// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CLUSTERING_GENERIC_RAFT_CORE_HPP_
#define CLUSTERING_GENERIC_RAFT_CORE_HPP_

#include <deque>
#include <set>
#include <map>

#include "errors.hpp"
#include <boost/optional.hpp>

#include "concurrency/auto_drainer.hpp"
#include "concurrency/new_mutex.hpp"
#include "concurrency/signal.hpp"
#include "concurrency/watchable.hpp"
#include "containers/uuid.hpp"
#include "time.hpp"

/* This file implements the Raft consensus algorithm, as described in the paper "In
Search of an Understandable Consensus Algorithm (Extended Version)" (2014) by Diego
Ongaro and John Ousterhout. Because of the complexity and subtlety of the Raft algorithm,
we follow the paper closely and refer back to it regularly. You are advised to have a
copy of the paper on hand when reading or modifying this file.

This file only contains the basic Raft algorithm itself; it doesn't contain any
networking or storage logic. Instead, it uses abstract interfaces to send and receive
network messages and write data to persistent storage. This both keeps this file as
as simple as possible and makes it easy to test the Raft algorithm using mocked-up
network and storage systems.

We support both log compaction and configuration changes.

The classes in this file are templatized on a types called `state_t`, which represents
the state machine that the Raft cluster manages. Operations on the state machine are
represented by a member type `state_t::change_t`. So `state_t::change_t` is the type that
is stored in the Raft log, and `state_t` is stored when taking a snapshot.

`state_t` and `state_t::change_t` must satisfy the following requirements:
  * Both must be default-constructable, destructable, copy- and move-constructable, copy-
    and move-assignable.
  * Both must support the `==` and `!=` operators.
  * `state_t` must have a method `void apply_change(const change_t &)` which applies the
    change to the `state_t`, mutating it in place. */

/* `raft_term_t` and `raft_log_index_t` are typedefs to improve the readability of the
code, by making it clearer what the meaning of a particular number is. */
typedef uint64_t raft_term_t;
typedef uint64_t raft_log_index_t;

/* Every member of the Raft cluster is identified by a `machine_id_t`. The Raft paper
uses integers for this purpose, but we use UUIDs because we have no reliable distributed
way of assigning integers, but we've already assigned a `machine_id_t` to each server in
the cluster. */

/* `raft_config_t` describes the set of members that are involved in the Raft cluster. */
class raft_config_t {
public:
    /* Regular members of the Raft cluster go in `voting_members`. `non_voting_members`
    is for members that should receive updates, but that don't count for voting purposes.
    */
    std::set<machine_id_t> voting_members, non_voting_members;

    /* Returns a list of all members, voting and non-voting. */
    std::set<machine_id_t> get_all_members() const {
        std::set<machine_id_t> members;
        members.insert(voting_members.begin(), voting_members.end());
        members.insert(non_voting_members.begin(), non_voting_members.end());
        return members;
    }

    /* Returns `true` if `members` constitutes a majority. */
    bool is_quorum(const std::set<machine_id_t> &members) const {
        size_t votes = 0;
        for (const machine_id_t &m : members) {
            votes += voting_members.count(m);
        }
        return (votes * 2 > voting_members.size());
    }

    /* Returns `true` if the given member can act as a leader. (Mostly this exists for
    consistency with `raft_complex_config_t`.) */
    bool is_valid_leader(const machine_id_t &member) const {
        return voting_members.count(member) == 1;
    }

    /* The equality and inequality operators are mostly for debugging */
    bool operator==(const raft_config_t &other) const {
        return voting_members == other.voting_members &&
            non_voting_members == other.non_voting_members;
    }
    bool operator!=(const raft_config_t &other) const {
        return !(*this == other);
    }
};

/* `raft_complex_config_t` can represent either a `raft_config_t` or a joint consensus of
an old and a new `raft_config_t`. */
class raft_complex_config_t {
public:
    /* For a regular configuration, `config` holds the configuration and `new_config` is
    empty. For a joint consensus configuration, `config` holds the old configuration and
    `new_config` holds the new configuration. */
    raft_config_t config;
    boost::optional<raft_config_t> new_config;

    bool is_joint_consensus() const {
        return static_cast<bool>(new_config);
    }

    std::set<machine_id_t> get_all_members() const {
        std::set<machine_id_t> members = config.get_all_members();
        if (is_joint_consensus()) {
            /* Raft paper, Section 6: "Log entries are replicated to all servers in both
            configurations." */
            std::set<machine_id_t> members2 = new_config->get_all_members();
            members.insert(members2.begin(), members2.end());
        }
        return members;
    }

    bool is_quorum(const std::set<machine_id_t> &members) const {
        /* Raft paper, Section 6: "Agreement (for elections and entry commitment)
        requires separate majorities from both the old and new configurations." */
        if (is_joint_consensus()) {
            return config.is_quorum(members) && new_config->is_quorum(members);
        } else {
            return config.is_quorum(members);
        }
    }

    bool is_valid_leader(const machine_id_t &member) const {
        /* Raft paper, Section 6: "Any server from either configuration may serve as
        leader." */
        return config.is_valid_leader(member) ||
            (is_joint_consensus() && new_config->is_valid_leader(member));
    }

    /* The equality and inequality operators are mostly for debugging */
    bool operator==(const raft_complex_config_t &other) const {
        return config == other.config && new_config == other.new_config;
    }
    bool operator!=(const raft_complex_config_t &other) const {
        return !(*this == other);
    }
};

/* `raft_log_entry_t` describes an entry in the Raft log. */
template<class state_t>
class raft_log_entry_t {
public:
    enum class type_t {
        /* A `regular` log entry is one with a `change_t`. So if `type` is `regular`,
        then `change` has a value but `configuration` is empty. */
        regular,
        /* A `configuration` log entry has a `raft_complex_config_t`. They are used to
        change the cluster configuration. See Section 6 of the Raft paper. So if `type`
        is `configuration`, then `configuration` has a value but `change` is empty. */
        configuration,
        /* A `noop` log entry does nothing and carries niether a `change_t` nor a
        `raft_configuration_t`. See Section 8 of the Raft paper. */
        noop
    };

    type_t type;
    raft_term_t term;
    /* Whether `change` and `configuration` are empty or not depends on the value of
    `type`. */
    boost::optional<typename state_t::change_t> change;
    boost::optional<raft_complex_config_t> configuration;
};

/* `raft_log_t` stores a slice of the Raft log. There are two situations where this shows
up in Raft: in an "AppendEntries RPC", and in each server's local state. The Raft paper
represents this as three separate variables, but grouping them together makes the
code clearer. */
template<class state_t>
class raft_log_t {
public:
    /* In an append-entries message, `prev_index` and `prev_term` correspond to the
    parameters that Figure 2 of the Raft paper calls `prevLogIndex` and `prevLogTerm`,
    and `entries` corresponds to the parameter that the Raft paper calls `entries`.

    In a server's local state, `prev_index` and `prev_term` correspond to the "last
    included index" and "last included term" variables as described in Section 7.
    `entries` corresponds to the `log` variable described in Figure 2. */

    raft_log_index_t prev_index;
    raft_term_t prev_term;
    std::deque<raft_log_entry_t<state_t> > entries;

    /* Return the latest index that is present in the log. If the log is empty, returns
    the index on which the log is based. */
    raft_log_index_t get_latest_index() const {
        return prev_index + entries.size();
    }

    /* Returns the term of the log entry at the given index. The index must either be
    present in the log or the last index before the log. */
    raft_term_t get_entry_term(raft_log_index_t index) const {
        guarantee(index >= prev_index, "the log doesn't go back this far");
        guarantee(index <= get_latest_index(), "the log doesn't go forward this far");
        if (index == prev_index) {
            return prev_term;
        } else {
            return get_entry_ref(index).term;
        }
    }

    /* Returns the entry in the log at the given index. */
    const raft_log_entry_t<state_t> &get_entry_ref(raft_log_index_t index) const {
        guarantee(index > prev_index, "the log doesn't go back this far");
        guarantee(index <= get_latest_index(), "the log doesn't go forward this far");
        return entries[index - prev_index - 1];
    }

    /* Deletes the log entry at the given index and all entries after it. */
    void delete_entries_from(raft_log_index_t index) {
        guarantee(index > prev_index, "the log doesn't go back this far");
        guarantee(index <= get_latest_index(), "the log doesn't go forward this far");
        entries.erase(entries.begin() + (index - prev_index - 1), entries.end());
    }

    /* Deletes the log entry at the given index and all entries before it. */
    void delete_entries_to(raft_log_index_t index) {
        guarantee(index > prev_index, "the log doesn't go back this far");
        guarantee(index <= get_latest_index(), "the log doesn't go forward this far");
        raft_term_t index_term = get_entry_term(index);
        entries.erase(entries.begin(), entries.begin() + (index - prev_index));
        prev_index = index;
        prev_term = index_term;
    }

    /* Appends the given entry ot the log. */
    void append(const raft_log_entry_t<state_t> &entry) {
        entries.push_back(entry);
    }
};

/* `raft_persistent_state_t` describes the information that each member of the Raft
cluster persists to stable storage. */
template<class state_t>
class raft_persistent_state_t {
public:
    /* `make_initial(s)` returns a `raft_persistent_state_t` for a member of a new Raft
    instance with starting state `initial_state` and configuration `initial_config`. The
    caller must ensure that every member of the new Raft cluster starts with the same
    values for these variables. */
    static raft_persistent_state_t make_initial(
        const state_t &initial_state,
        const raft_config_t &initial_config);

    /* `make_join()` returns a `raft_persistent_state_t` for a Raft member that will be
    joining an already-established Raft cluster. A Raft member initialized this way
    should be added to the cluster as a non-voting member, and not made a voting member
    until it has received a snapshot. */
    static raft_persistent_state_t make_join();

    /* `current_term` and `voted_for` correspond to the variables with the same names in
    Figure 2 of the Raft paper. */
    raft_term_t current_term;
    machine_id_t voted_for;

    /* `snapshot_state` corresponds to the stored snapshotted state, as described in
    Section 7. An empty `boost::optional<state_t>()` is the initial state of the Raft
    cluster, although in practice it will be initialized almost immediately. */
    boost::optional<state_t> snapshot_state;

    /* `snapshot_configuration` corresponds to the stored snapshotted configuration, as
    described in Section 7. This implementation deviates from the Raft paper in that we
    allow non-voting members to not know the cluster configuration until they receive
    their first snapshot. This should be safe because they are non-voting members. If a
    `raft_member_t` sees that its `snapshot_configuration` is empty, it assumes that it
    is a non-voting member and will never try to become leader. */
    boost::optional<raft_complex_config_t> snapshot_configuration;

    /* `log.prev_index` and `log.prev_term` correspond to the "last included index" and
    "last included term" as described in Section 7. `log.entries` corresponds to the
    `log` variable in Figure 2. */
    raft_log_t<state_t> log;
};

/* `raft_storage_interface_t` is an abstract class that `raft_member_t` uses to store
data on disk. */
template<class state_t>
class raft_storage_interface_t {
public:
    /* `write_persistent_state()` writes the state of the Raft member to stable storage.
    It does not return until the state is safely stored. The values stored with
    `write_state()` will be passed to the `raft_member_t` constructor when the Raft
    member is restarted. */
    virtual void write_persistent_state(
        const raft_persistent_state_t<state_t> &persistent_state,
        signal_t *interruptor) = 0;

    /* If writing the state becomes a performance bottleneck, we could implement a
    variant that only rewrites part of the state. In particular, we often need to append
    a few entries to the log but don't need to make any other changes. */

protected:
    virtual ~raft_storage_interface_t() { }
};

/* `raft_request_vote_rpc_t` describes the parameters to the "RequestVote RPC" described
in Figure 2 of the Raft paper. */
class raft_request_vote_rpc_t {
public:
    /* `term`, `candidate_id`, `last_log_index`, and `last_log_term` correspond to the
    parameters with the same names in the Raft paper. */
    raft_term_t term;
    machine_id_t candidate_id;
    raft_log_index_t last_log_index;
    raft_term_t last_log_term;
};

/* `raft_request_vote_reply_t` describes the information returned from the "RequestVote
RPC" described in Figure 2 of the Raft paper. */
class raft_request_vote_reply_t {
public:
    raft_term_t term;
    bool vote_granted;
};

/* `raft_install_snapshot_rpc_t` describes the parameters of the "InstallSnapshot RPC"
described in Figure 13 of the Raft paper. */
template<class state_t>
class raft_install_snapshot_rpc_t {
public:
    /* `term`, `leader_id`, `last_included_index`, and `last_included_term`
    correspond to the parameters with the same names in the Raft paper. In the Raft
    paper, the content of the snapshot is sent as a series of binary blobs, but we don't
    want to do that; instead, we send the `state_t` and `raft_configuration_t` directly.
    So our `snapshot_state` and `snapshot_configuration` parameters replace the `offset`,
    `data`, and `done` parameters of the Raft paper. */
    raft_term_t term;
    machine_id_t leader_id;
    raft_log_index_t last_included_index;
    raft_term_t last_included_term;
    state_t snapshot_state;
    raft_complex_config_t snapshot_configuration;
};

/* `raft_install_snapshot_reply_t` describes in the information returned from the
"InstallSnapshot RPC" described in Figure 13 of the Raft paper. */
class raft_install_snapshot_reply_t {
public:
    raft_term_t term;
};

/* `raft_append_entries_rpc_t` describes the parameters of the "AppendEntries RPC"
described in Figure 2 of the Raft paper. */
template<class state_t>
class raft_append_entries_rpc_t {
public:
    /* `term`, `leader_id`, and `leader_commit` correspond to the parameters with the
    same names in the Raft paper. `entries` corresponds to three of the paper's
    variables: `prevLogIndex`, `prevLogTerm`, and `entries`. */
    raft_term_t term;
    machine_id_t leader_id;
    raft_log_t<state_t> entries;
    raft_log_index_t leader_commit;
};

/* `raft_append_entries_reply_t` describes the information returned from the
"AppendEntries RPC" described in Figure 2 of the Raft paper. */
class raft_append_entries_reply_t {
public:
    raft_term_t term;
    bool success;
};

/* `raft_network_interface_t` is the abstract class that `raft_member_t` uses to send
messages over the network. */
template<class state_t>
class raft_network_interface_t {
public:
    /* The `send_*_rpc()` methods all follow these rules:
      * They send an RPC message to the Raft member indicated in the `dest` field.
      * The message will be delivered by calling the `on_*_rpc()` method on the
        `raft_member_t` in question.
      * If the RPC is delivered successfully, `send_*_rpc()` will return `true`, and the
        results will be stored in the `*_out` variables.
      * If something goes wrong, `send_*_rpc()` will return `false`. The RPC may or may
        not have been delivered. The caller should wait until the Raft member is present
        in `get_connected_members()` before trying again.
      * If the interruptor is pulsed, it throws `interrupted_exc_t`. The RPC may or may
        may not have been delivered. */

    virtual bool send_request_vote_rpc(
        const machine_id_t &dest,
        const raft_request_vote_rpc_t &params,
        signal_t *interruptor,
        raft_request_vote_reply_t *reply_out) = 0;

    virtual bool send_install_snapshot_rpc(
        const machine_id_t &dest,
        const raft_install_snapshot_rpc_t<state_t> &params,
        signal_t *interruptor,
        raft_install_snapshot_reply_t *reply_out) = 0;

    virtual bool send_append_entries_rpc(
        const machine_id_t &dest,
        const raft_append_entries_rpc_t<state_t> &params,
        signal_t *interruptor,
        raft_append_entries_reply_t *reply_out) = 0;

    /* `get_connected_members()` returns the set of all Raft members for which an RPC is
    likely to succeed. */
    virtual clone_ptr_t<watchable_t<std::set<machine_id_t> > >
        get_connected_members() = 0;

protected:
    virtual ~raft_network_interface_t() { }
};

/* `raft_member_t` is responsible for managing the activity of a single member of the
Raft cluster. */
template<class state_t>
class raft_member_t : public home_thread_mixin_debug_only_t
{
public:
    raft_member_t(
        const machine_id_t &this_member_id,
        raft_storage_interface_t<state_t> *storage,
        raft_network_interface_t<state_t> *network,
        const raft_persistent_state_t<state_t> &persistent_state);

    ~raft_member_t();

    /* Note that if a method on `raft_member_t` is interrupted, the `raft_member_t` will
    be left in an undefined internal state. Therefore, the destructor should be called
    after the interruptor has been pulsed. (However, even though the internal state
    is undefined, the interrupted method call will not make invalid RPC calls or write
    invalid data to persistent storage.) */

    /* `get_initialized_signal()` returns a signal that is pulsed if we have a valid
    state. The only time it isn't pulsed is when we've just joined an existing Raft
    cluster as a new member, and we haven't received the initial state yet. */
    signal_t *get_initialized_signal() {
        assert_thread();
        return &initialized_cond;
    }

    /* `get_state_machine()` tracks the current state of the state machine. It's illegal
    to call this before `get_initialized_signal()` is pulsed. */
    clone_ptr_t<watchable_t<state_t> > get_state_machine() {
        assert_thread();
        guarantee(initialized_cond.is_pulsed());
        return state_machine.get_watchable();
    }

    /* TODO: These user-facing APIs are inadequate. We'll probably need:
      * A way to block until a newly-created Raft cluster has elected a leader and is
        ready for input.
      * For queries initiated by the user, we'll want to be able to know if they
        succeeded or failed. This should report "failed" if anything delays the query
        significantly, such as if a new master is elected before the query is committed,
        or if the master is no longer in contact with a majority.
      * A way to observe the state of the Raft cluster before initiating a change.
        Specifically, it would observe the "bleeding edge" state after everything in the
        log has been applied, not the committed state. This way we can enforce rules for
        what changes are allowed following what states.
    But I don't want to implement anything until I have a better sense of how these APIs
    will end up being used. So I'll revisit this later. */

    /* Returns the Raft member that this member thinks is the leader, or `nil_uuid()` if
    this member doesn't know of any leader. */
    machine_id_t get_leader() {
        assert_thread();
        return current_term_leader_id;
    }

    /* `propose_change_if_leader()` tries to perform the given change if this Raft member
    is the leader. A return value of `true` means the change is being processed, but it
    hasn't necessarily been committed and won't necessarily ever be. `false` means we are
    not the leader or something went wrong. */
    bool propose_change_if_leader(
        const typename state_t::change_t &change,
        signal_t *interruptor);

    /* `propose_config_change_if_leader()` is like `propose_change_if_leader()` except
    that it proposes a reconfiguration instead of a `change_t`. */
    bool propose_config_change_if_leader(
        const raft_config_t &configuration,
        signal_t *interruptor);

    /* The `on_*_rpc()` methods are called when a Raft member calls a `send_*_rpc()`
    method on their `raft_network_and_storage_interface_t`. */
    void on_request_vote_rpc(
        const raft_request_vote_rpc_t &rpc,
        signal_t *interruptor,
        raft_request_vote_reply_t *reply_out);
    void on_install_snapshot_rpc(
        const raft_install_snapshot_rpc_t<state_t> &rpc,
        signal_t *interruptor,
        raft_install_snapshot_reply_t *reply_out);
    void on_append_entries_rpc(
        const raft_append_entries_rpc_t<state_t> &rpc,
        signal_t *interruptor,
        raft_append_entries_reply_t *reply_out);

    /* `check_invariants()` asserts that the given collection of Raft cluster members are
    in a valid, consistent state. This may block, because it needs to acquire each
    member's mutex, but it will not modify anything. Since this requires direct access to
    each member of the Raft cluster, it's only useful for testing. */
    static void check_invariants(
        const std::set<raft_member_t<state_t> *> &members);

private:
    enum class mode_t {
        follower,
        candidate,
        leader
    };

    /* These are the minimum and maximum election timeouts. In section 5.6, the Raft
    paper suggests that a typical election timeout should be somewhere between 10ms and
    500ms. We use somewhat larger numbers to reduce server traffic, at the cost of longer
    periods of unavailability when a master dies. */
    static const int32_t election_timeout_min_ms = 1000,
                         election_timeout_max_ms = 2000;

    /* TODO: We should probably deviate from the Raft paper by using the network layer's
    disconnect detection instead of timeouts to detect a dead leader. This will make
    elections much faster and also make us less sensitive to timing. However, this will
    involve adding a new RPC, for a master to inform followers that it is stepping down.
    */

    /* This is the amount of time the server waits between sending heartbeats. It should
    be much shorter than the election timeout. */
    static const int32_t heartbeat_interval_ms = 500;

    /* Note: Methods prefixed with `follower_`, `candidate_`, or `leader_` are methods
    that are only used when in that state. This convention will hopefully make the code
    slightly clearer. */

    /* Asserts that all of the invariants that can be checked locally hold true. This
    doesn't block or modify anything. It should be safe to call it at any time (except
    when in between modifying two variables that should remain consistent with each
    other, of course). In general we call it whenever we acquire or release the mutex,
    because we know that the variables should be consistent at those times. */
    void check_invariants(const new_mutex_acq_t *mutex_acq);

    /* `on_watchdog_timer()` is called periodically. If we're a follower and we haven't
    heard from a leader within the election timeout, it starts a new election by spawning
    `candidate_and_leader_coro()`. */
    void on_watchdog_timer();

    /* `update_term()` sets the term to `new_term` and resets all per-term variables. It
    assumes that its caller will flush persistent state to stable storage eventually
    after it returns. */
    void update_term(raft_term_t new_term,
                     const new_mutex_acq_t *mutex_acq);

    /* When we change the commit index we have to also apply changes to the state
    machine. `update_commit_index()` handles that automatically. It assumes that its
    caller will flush persistent state to stable storage eventually after it returns. */
    void update_commit_index(raft_log_index_t new_commit_index,
                             const new_mutex_acq_t *mutex_acq);

    /* When we change `match_index` we might have to update `commit_index` as well.
    `leader_update_match_index()` handles that automatically. It may flush persistent
    state to stable storage before it returns. */
    void leader_update_match_index(
        /* Since `match_index` lives on the stack of `candidate_and_leader_coro()`, we
        have to pass in a pointer. */
        std::map<machine_id_t, raft_log_index_t> *match_index,
        machine_id_t key,
        raft_log_index_t new_value,
        const new_mutex_acq_t *mutex_acq,
        signal_t *interruptor);

    /* `candidate_or_leader_become_follower()` moves us from the `candidate` or `leader`
    state to `follower` state. It kills `candidate_and_leader_coro()` and blocks until it
    exits. */
    void candidate_or_leader_become_follower(const new_mutex_acq_t *mutex_acq);

    /* `follower_become_candidate()` moves us from the `follower` state to the
    `candidate` state. It spawns `candidate_and_leader_coro()`.*/
    void follower_become_candidate(const new_mutex_acq_t *mutex_acq);

    /* `candidate_and_leader_coro()` contains most of the candidate- and leader-specific
    logic. It runs in a separate coroutine for as long as we are a candidate or leader.
    */
    void candidate_and_leader_coro(
        /* A `new_mutex_acq_t` allocated on the heap. `candidate_and_leader_coro()` takes
        ownership of it. */
        new_mutex_acq_t *mutex_acq_on_heap,
        /* To make sure that `candidate_and_leader_coro` stops before the `raft_member_t`
        is destroyed. This is also used by `candidate_or_leader_become_follower()` to
        interrupt `candidate_and_leader_coro()`. */
        auto_drainer_t::lock_t leader_keepalive);

    /* `candidate_run_election()` is a helper function for `candidate_and_leader_coro()`.
    It sends out request-vote RPCs and wait for us to get enough votes. It blocks until
    we are elected or `cancel_signal` is pulsed. The caller is responsible for detecting
    the case where another leader is elected and also for detecting the case where the
    election times out, and pulsing `cancel_signal`. It returns `true` if we were
    elected. */
    bool candidate_run_election(
        /* Note that `candidate_run_election()` may temporarily release `mutex_acq`, but
        it will always be holding the lock when `run_election()` returns. But if
        `interruptor` is pulsed it will throw `interrupted_exc_t` and not reacquire the
        lock. */
        scoped_ptr_t<new_mutex_acq_t> *mutex_acq,
        signal_t *cancel_signal,
        signal_t *interruptor);

    /* `leader_spawn_update_coros()` is a helper function for
    `candidate_and_leader_coro()` that spawns or kills instances of `run_updates()` as
    necessary to ensure that there is always one for each cluster member. */
    void leader_spawn_update_coros(
        /* The value of `nextIndex` to use for each newly connected peer. */
        raft_log_index_t initial_next_index,
        /* A map containing `matchIndex` for each connected peer, as described in Figure
        2 of the Raft paper. This lives on the stack in `candidate_and_leader_coro()`. */
        std::map<machine_id_t, raft_log_index_t> *match_indexes,
        /* A map containing an `auto_drainer_t` for each running update coroutine. */
        std::map<machine_id_t, scoped_ptr_t<auto_drainer_t> > *update_drainers,
        const new_mutex_acq_t *mutex_acq);

    /* `leader_send_updates()` is a helper function for `candidate_and_leader_coro()`;
    `leader_spawn_update_coros()` spawns one in a new coroutine for each peer. It pushes
    install-snapshot RPCs and/or append-entry RPCs out to the given peer until
    `update_keepalive.get_drain_signal()` is pulsed. */
    void leader_send_updates(
        const machine_id_t &peer,
        raft_log_index_t initial_next_index,
        std::map<machine_id_t, raft_log_index_t> *match_indexes,
        auto_drainer_t::lock_t update_keepalive);

    /* `leader_continue_reconfiguration()` is a helper function for
    `candidate_and_leader_coro()`. It checks if we have completed the first phase of a
    reconfiguration (by committing a joint consensus configuration) and if so, it starts
    the second phase by committing the new configuration. It also checks if we have
    completed the second phase and if so, it makes us step down. */
    void leader_continue_reconfiguration(
        const new_mutex_acq_t *mutex_acq,
        signal_t *interruptor);

    /* `candidate_or_leader_note_term()` is a helper function for
    `candidate_run_election()` and `leader_send_updates()`. If the given term is greater
    than the current term, it updates the current term and interrupts
    `candidate_and_leader_coro()`. It returns `true` if the term was changed. */
    bool candidate_or_leader_note_term(
        raft_term_t term,
        const new_mutex_acq_t *mutex_acq);

    /* `leader_append_log_entry()` is a helper for `propose_change_if_leader()` and
    `propose_config_change_if_leader()`. It adds an entry to the log but doesn't wait for
    the entry to be committed. It flushes persistent state to stable storage. */
    void leader_append_log_entry(
        const raft_log_entry_t<state_t> &log_entry,
        const new_mutex_acq_t *mutex_acq,
        signal_t *interruptor);

    /* Returns the configuration that we should use for determining if we have a quorum
    or not. */
    raft_complex_config_t get_configuration();

    /* The member ID of the member of the Raft cluster represented by this
    `raft_member_t`. */
    const machine_id_t this_member_id;

    raft_storage_interface_t<state_t> *const storage;
    raft_network_interface_t<state_t> *const network;

    /* This stores all of the state variables of the Raft member that need to be written
    to stable storage when they change. We end up writing `ps.*` a lot, which is why the
    name is so abbreviated. */
    raft_persistent_state_t<state_t> ps;

    /* `state_machine` and `initialized_cond` together describe the "state machine" that
    the Raft member is managing. If `initialized_cond` is unpulsed, then the state
    machine is in the uninitialized state, and the contents of `state_machine` are
    meaningless; if `initialized_cond` is pulsed, then the state machine is in an
    initialized state, and `state_machine` stores that state. In the context of the
    `raft_persistent_state_t` we represent this as a `boost::optional<state_t>`, but here
    we want to store it in a form that is easier for users of `raft_member_t` to work
    with. */
    watchable_variable_t<state_t> state_machine;
    cond_t initialized_cond;

    /* `commit_index` and `last_applied` correspond to the volatile state variables with
    the same names in Figure 2 of the Raft paper. */
    raft_log_index_t commit_index, last_applied;

    /* Only `candidate_and_leader_coro()` should ever change `mode` */
    mode_t mode;

    /* `current_term_leader_id` is the ID of the member that is leader during this term.
    If we haven't seen any node acting as leader this term, it's `nil_uuid()`. We use it
    to redirect clients as described in Figure 2 and Section 8. */
    machine_id_t current_term_leader_id;

    /* `last_heard_from_leader` is the time that we last heard from a leader or
    candidate. `on_watchdog_timer()` consults it to see if we should start an election.
    */
    microtime_t last_heard_from_leader;

    /* This mutex ensures that operations don't interleave in confusing ways. Each RPC
    acquires this mutex when it begins and releases it when it returns. Also, if
    `candidate_and_leader_coro()` is running, it holds this mutex when actively
    manipulating state and releases it when waiting. In general we don't hold the mutex
    when responding to an interruptor. */
    new_mutex_t mutex;

    /* This mutex assertion controls writes to the Raft log and associated state.
    Specifically, anything writing to `ps.log`, `ps.snapshot`, `state_machine`,
    `commit_index`, or `last_applied` should hold this mutex assertion.
    If we are follower, `on_append_entries_rpc()` and `on_install_snapshot_rpc()` acquire
    this temporarily; if we are candidate or leader, `candidate_and_leader_coro()` holds
    this at all times. */
    mutex_assertion_t log_mutex;

    /* When we are leader, `update_watchers` is a set of conds that should be pulsed
    every time we append something to the log or update `commit_index`. If we are not
    leader, this is empty and unused. */
    std::set<cond_t *> update_watchers;

    /* This makes sure that `candidate_and_leader_coro()` stops when the `raft_member_t`
    is destroyed. It's in a `scoped_ptr_t` so that
    `candidate_or_leader_become_follower()` can destroy it to kill
    `candidate_and_leader_coro()`. */
    scoped_ptr_t<auto_drainer_t> leader_drainer;

    /* Occasionally we have to spawn miscellaneous coroutines. This makes sure that they
    all get stopped before the `raft_member_t` is destroyed. It's in a `scoped_ptr_t` so
    that the destructor can destroy it early. */
    scoped_ptr_t<auto_drainer_t> drainer;

    /* This periodically calls `on_watchdog_timer()` to check if we need to start a new
    election. It's in a `scoped_ptr_t` so that the destructor can destroy it early. */
    scoped_ptr_t<repeating_timer_t> watchdog_timer;
};

#endif /* CLUSTERING_GENERIC_RAFT_CORE_HPP_ */

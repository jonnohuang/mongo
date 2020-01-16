/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <cstddef>
#include <functional>

#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/abstract_oplog_fetcher.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace repl {

extern FailPoint stopReplProducer;

/**
 * The oplog fetcher, once started, reads operations from a remote oplog using a tailable cursor.
 *
 * The initial find command is generated from last fetched optime and may contain the current term
 * depending on the replica set config provided.
 *
 * Forwards metadata in each find/getMore response to the data replicator external state.
 *
 * Performs additional validation on first batch of operations returned from the query to ensure we
 * are able to continue from our last known fetched operation.
 *
 * Validates each batch of operations.
 *
 * Pushes operations from each batch of operations onto a buffer using the "enqueueDocumentsFn"
 * function.
 *
 * Issues a getMore command after successfully processing each batch of operations.
 *
 * When there is an error or when it is not possible to issue another getMore request, calls
 * "onShutdownCallbackFn" to signal the end of processing.
 *
 * This class subclasses AbstractOplogFetcher which takes care of scheduling the Fetcher and
 * `getMore` commands, and handles restarting on errors.
 */
class OplogFetcher : public AbstractOplogFetcher {
    OplogFetcher(const OplogFetcher&) = delete;
    OplogFetcher& operator=(const OplogFetcher&) = delete;

public:
    static Seconds kDefaultProtocolZeroAwaitDataTimeout;

    /**
     * Statistics on current batch of operations returned by the fetcher.
     */
    struct DocumentsInfo {
        size_t networkDocumentCount = 0;
        size_t networkDocumentBytes = 0;
        size_t toApplyDocumentCount = 0;
        size_t toApplyDocumentBytes = 0;
        OpTime lastDocument = OpTime();
    };

    /**
     * An enum that indicates if we want to skip the first document during oplog fetching or not.
     * Currently, the only time we don't want to skip the first document is during initial sync
     * if the sync source has a valid oldest active transaction optime, as we need to include
     * the corresponding oplog entry when applying.
     */
    enum class StartingPoint { kSkipFirstDoc, kEnqueueFirstDoc };

    /**
     * Type of function that accepts a pair of iterators into a range of operations
     * within the current batch of results and copies the operations into
     * a buffer to be consumed by the next stage of the replication process.
     *
     * Additional information on the operations is provided in a DocumentsInfo
     * struct.
     */
    using EnqueueDocumentsFn = std::function<Status(Fetcher::Documents::const_iterator begin,
                                                    Fetcher::Documents::const_iterator end,
                                                    const DocumentsInfo& info)>;

    /**
     * Validates documents in current batch of results returned from tailing the remote oplog.
     * 'first' should be set to true if this set of documents is the first batch returned from the
     * query.
     * On success, returns statistics on operations.
     */
    static StatusWith<DocumentsInfo> validateDocuments(
        const Fetcher::Documents& documents,
        bool first,
        Timestamp lastTS,
        StartingPoint startingPoint = StartingPoint::kSkipFirstDoc);

    /**
     * Invariants if validation fails on any of the provided arguments.
     */
    OplogFetcher(executor::TaskExecutor* executor,
                 OpTime lastFetched,
                 HostAndPort source,
                 NamespaceString nss,
                 ReplSetConfig config,
                 std::size_t maxFetcherRestarts,
                 int requiredRBID,
                 bool requireFresherSyncSource,
                 DataReplicatorExternalState* dataReplicatorExternalState,
                 EnqueueDocumentsFn enqueueDocumentsFn,
                 OnShutdownCallbackFn onShutdownCallbackFn,
                 const int batchSize,
                 StartingPoint startingPoint = StartingPoint::kSkipFirstDoc);

    OplogFetcher(executor::TaskExecutor* executor,
                 OpTime lastFetched,
                 HostAndPort source,
                 NamespaceString nss,
                 ReplSetConfig config,
                 std::unique_ptr<OplogFetcherRestartDecision> oplogFetcherRestartDecision,
                 int requiredRBID,
                 bool requireFresherSyncSource,
                 DataReplicatorExternalState* dataReplicatorExternalState,
                 EnqueueDocumentsFn enqueueDocumentsFn,
                 OnShutdownCallbackFn onShutdownCallbackFn,
                 const int batchSize,
                 StartingPoint startingPoint = StartingPoint::kSkipFirstDoc);

    virtual ~OplogFetcher();

    // ================== Test support API ===================

    /**
     * Returns metadata object sent in remote commands.
     */
    BSONObj getMetadataObject_forTest() const;

    /**
     * Returns timeout for remote commands to complete.
     */
    Milliseconds getRemoteCommandTimeout_forTest() const;

    /**
     * Returns the await data timeout used for the "maxTimeMS" field in getMore command requests.
     */
    Milliseconds getAwaitDataTimeout_forTest() const;

private:
    BSONObj _makeFindCommandObject(const NamespaceString& nss,
                                   OpTime lastOpTimeFetched,
                                   Milliseconds findMaxTime) const override;

    BSONObj _makeMetadataObject() const override;

    Milliseconds _getGetMoreMaxTime() const override;

    /**
     * This function is run by the AbstractOplogFetcher on a successful batch of oplog entries.
     */
    StatusWith<BSONObj> _onSuccessfulBatch(const Fetcher::QueryResponse& queryResponse) override;

    // The metadata object sent with the Fetcher queries.
    const BSONObj _metadataObject;

    // Rollback ID that the sync source is required to have after the first batch.
    int _requiredRBID;

    // A boolean indicating whether we should error if the sync source is not ahead of our initial
    // last fetched OpTime on the first batch. Most of the time this should be set to true,
    // but there are certain special cases, namely during initial sync, where it's acceptable for
    // our sync source to have no ops newer than _lastFetched.
    bool _requireFresherSyncSource;

    DataReplicatorExternalState* const _dataReplicatorExternalState;
    const EnqueueDocumentsFn _enqueueDocumentsFn;
    const Milliseconds _awaitDataTimeout;
    const int _batchSize;

    // Indicates if we want to skip the first document during oplog fetching or not.
    StartingPoint _startingPoint;
};

/**
 * The oplog fetcher, once started, reads operations from a remote oplog using a tailable,
 * awaitData, exhaust cursor.
 *
 * The initial `find` command is generated from the last fetched optime.
 *
 * Using RequestMetadataWriter and ReplyMetadataReader, the sync source will forward metadata in
 * each response that will be sent to the data replicator external state.
 *
 * Performs additional validation on first batch of operations returned from the query to ensure we
 * are able to continue from our last known fetched operation.
 *
 * Validates each batch of operations to make sure that none of the oplog entries are out of order.
 *
 * Collect stats about all the batches received to be able to report in serverStatus metrics.
 *
 * Pushes operations from each batch of operations onto a buffer using the "enqueueDocumentsFn"
 * function.
 *
 * When there is an error, it will create a new cursor by issuing a new `find` command to the sync
 * source. If the sync source is no longer eligible or the OplogFetcher was shutdown, calls
 * "onShutdownCallbackFn" to signal the end of processing.
 *
 * An oplog fetcher is an abstract async component, which takes care of startup and shutdown logic.
 *
 * TODO SERVER-45574: edit or remove this flowchart when the NewOplogFetcher is implemented.
 *
 * NewOplogFetcher flowchart:
 *
 *             _runQuery()
 *                  |
 *                  |
 *                  +---------+
 *                            |
 *                            |
 *                            V
 *                    _createNewCursor()
 *                            |
 *                            |
 *                            +<--------------------------+
 *                            |                           ^
 *                            |                           |
 *                      _getNextBatch()                   |
 *                        |       |                       |
 *                        |       |                       |
 *  (unsuccessful batch   |       | (successful batch)    |
 *       or error)        |       |                       |
 *                        |       V                       |
 *                        |  _onSuccessfulBatch()         |
 *                        |       |                       |
 *                        |       |                       |
 *                        |       |                       |
 *                        V       |                       |
 *            _createNewCursor()  |                       |
 *                        |       |                       |
 *                        |       |                       |
 *                        +---V---+                       |
 *                            |                           |
 *                            |                           |
 *                            +-------------------------->+
 *
 */
class NewOplogFetcher : public AbstractAsyncComponent {
    NewOplogFetcher(const OplogFetcher&) = delete;
    NewOplogFetcher& operator=(const OplogFetcher&) = delete;

public:
    /**
     * Type of function called by the oplog fetcher on shutdown with the final oplog fetcher status.
     *
     * The status will be Status::OK() if we have processed the last batch of operations from the
     * cursor.
     *
     * This function will be called 0 times if startup() fails and at most once after startup()
     * returns success.
     */
    using OnShutdownCallbackFn = std::function<void(const Status& shutdownStatus)>;

    /**
     * Container for BSON documents extracted from cursor results.
     */
    using Documents = std::vector<BSONObj>;

    /**
     * An enum that indicates if we want to skip the first document during oplog fetching or not.
     * Currently, the only time we don't want to skip the first document is during initial sync
     * if the sync source has a valid oldest active transaction optime, as we need to include
     * the corresponding oplog entry when applying.
     */
    enum class StartingPoint { kSkipFirstDoc, kEnqueueFirstDoc };

    /**
     * Statistics on current batch of operations returned by the sync source.
     */
    struct DocumentsInfo {
        size_t networkDocumentCount = 0;
        size_t networkDocumentBytes = 0;
        size_t toApplyDocumentCount = 0;
        size_t toApplyDocumentBytes = 0;
        OpTime lastDocument = OpTime();
    };

    /**
     * Type of function that accepts a pair of iterators into a range of operations
     * within the current batch of results and copies the operations into
     * a buffer to be consumed by the next stage of the replication process.
     *
     * Additional information on the operations is provided in a DocumentsInfo
     * struct.
     */
    using EnqueueDocumentsFn = std::function<Status(
        Documents::const_iterator begin, Documents::const_iterator end, const DocumentsInfo& info)>;

    class OplogFetcherRestartDecision {
    public:
        OplogFetcherRestartDecision(){};

        virtual ~OplogFetcherRestartDecision() = 0;

        /**
         * Defines which situations the oplog fetcher will restart after encountering an error.
         * Called when getting the next batch failed for some reason.
         */
        virtual bool shouldContinue(NewOplogFetcher* fetcher, Status status) = 0;

        /**
         * Called when a batch was successfully fetched to reset any state needed to track restarts.
         */
        virtual void fetchSuccessful(NewOplogFetcher* fetcher) = 0;
    };

    class OplogFetcherRestartDecisionDefault : public OplogFetcherRestartDecision {
    public:
        OplogFetcherRestartDecisionDefault(std::size_t maxRestarts) : _maxRestarts(maxRestarts){};

        bool shouldContinue(NewOplogFetcher* fetcher, Status status) final;

        void fetchSuccessful(NewOplogFetcher* fetcher) final;

        ~OplogFetcherRestartDecisionDefault(){};

    private:
        NewOplogFetcher* _newOplogFetcher;

        // Restarts since the last successful oplog query response.
        std::size_t _numRestarts = 0;

        const std::size_t _maxRestarts;
    };

    /**
     * Invariants if validation fails on any of the provided arguments.
     */
    NewOplogFetcher(executor::TaskExecutor* executor,
                    OpTime lastFetched,
                    HostAndPort source,
                    ReplSetConfig config,
                    std::unique_ptr<OplogFetcherRestartDecision> oplogFetcherRestartDecision,
                    int requiredRBID,
                    bool requireFresherSyncSource,
                    DataReplicatorExternalState* dataReplicatorExternalState,
                    EnqueueDocumentsFn enqueueDocumentsFn,
                    OnShutdownCallbackFn onShutdownCallbackFn,
                    const int batchSize,
                    StartingPoint startingPoint = StartingPoint::kSkipFirstDoc);

    virtual ~NewOplogFetcher();

    /**
     * Validates documents in current batch of results returned from tailing the remote oplog.
     * 'first' should be set to true if this set of documents is the first batch returned from the
     * query.
     * On success, returns statistics on operations.
     */
    static StatusWith<DocumentsInfo> validateDocuments(
        const Documents& documents,
        bool first,
        Timestamp lastTS,
        StartingPoint startingPoint = StartingPoint::kSkipFirstDoc);


    /**
     * Prints out the status and settings of the oplog fetcher.
     */
    std::string toString() const;

    // ================== Test support API ===================

    /**
     * Returns the `find` query run on the sync source's oplog.
     */
    BSONObj getFindQuery_forTest() const;

    /**
     * Returns the OpTime of the last oplog entry fetched and processed.
     */
    OpTime getLastOpTimeFetched_forTest() const;

    /**
     * Returns the await data timeout used for the "maxTimeMS" field in getMore command requests.
     */
    Milliseconds getAwaitDataTimeout_forTest() const;

private:
    // =============== AbstractAsyncComponent overrides ================

    /**
     * Schedules the _runQuery function to run in a separate thread.
     */
    Status _doStartup_inlock() noexcept override;

    /**
     * Shuts down the DBClientCursor and DBClientConnection. Uses the connection's
     * shutdownAndDisallowReconnect function to interrupt it.
     */
    void _doShutdown_inlock() noexcept override;

    Mutex* _getMutex() noexcept override;

    // ============= End AbstractAsyncComponent overrides ==============

    /**
     * Creates a DBClientConnection and executes a query to retrieve oplog entries from this node's
     * sync source. This will create a tailable, awaitData, exhaust cursor which will be used until
     * the cursor fails or OplogFetcher is shut down. For each batch returned by the upstream node,
     * _onSuccessfulBatch will be called with the response.
     *
     * In the case of any network or response errors, this method will close the cursor and restart
     * a new one. If OplogFetcherRestartDecision's shouldContinue function indicates it should not
     * create a new cursor, it will call _finishCallback.
     */
    void _runQuery(const executor::TaskExecutor::CallbackArgs& callbackData);

    /**
     * Executes a `find` query on the sync source's oplog and establishes a tailable, awaitData,
     * exhaust cursor. If it is not successful in creating a new cursor, it will retry based on the
     * OplogFetcherRestartDecision's shouldContinue function.
     *
     * Before running the query, it will set a RequestMetadataWriter to modify the request to
     * include $oplogQueryData and $replData. If will also set a ReplyMetadataReader to parse the
     * response for the metadata field.
     */
    Status _createNewCursor();

    /**
     * This function will create the `find` query to issue to the sync source. It is provided with
     * the last OpTime fetched so that it can begin from the middle of the oplog.
     */
    BSONObj _makeFindQuery(OpTime lastOpTimeFetched, Milliseconds findMaxTime) const;

    /**
     * Gets the next batch from the exhaust cursor.
     *
     * If there was an error getting the next batch, checks _oplogFetcherRestartDecision's
     * shouldContinue function to see if it should create a new cursor and if so, calls
     * _createNewCursor.
     */
    StatusWith<Documents> _getNextBatch();

    /**
     * Function called by the oplog fetcher when it gets a successful batch from the sync source.
     * This will also process the metadata received from the response.
     *
     * On failure returns a status that will be passed to _finishCallback.
     */
    Status _onSuccessfulBatch(const Documents& documents);

    /**
     * Notifies caller that the oplog fetcher has completed processing operations from the remote
     * oplog using the "_onShutdownCallbackFn".
     */
    void _finishCallback(Status status);

    /**
     * Returns how long the `find` command should wait before timing out.
     */
    Milliseconds _getInitialFindMaxTime() const;

    /**
     * Returns how long the `find` command should wait before timing out, if we are retrying the
     * `find` due to an error. This timeout should be considerably smaller than our initial oplog
     * `find` time, since a communication failure with an upstream node may indicate it is
     * unreachable.
     */
    Milliseconds _getRetriedFindMaxTime() const;

    /**
     * Returns the OpTime of the last oplog entry fetched and processed.
     */
    OpTime _getLastOpTimeFetched() const;

    // Protects member data of this OplogFetcher.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("OplogFetcher::_mutex");

    // Sync source to read from.
    const HostAndPort _source;

    // Namespace of the oplog to read.
    const NamespaceString _nss = NamespaceString::kRsOplogNamespace;

    // Rollback ID that the sync source is required to have after the first batch.
    int _requiredRBID;

    // Indicates whether the current batch is the first received via this cursor.
    bool _firstBatch = true;

    // In the case of an error, this will help decide if a new cursor should be created or the
    // oplog fetcher should be shut down.
    std::unique_ptr<OplogFetcherRestartDecision> _oplogFetcherRestartDecision;

    // Function to call when the oplog fetcher shuts down.
    OnShutdownCallbackFn _onShutdownCallbackFn;

    // Used to keep track of the last oplog entry read and processed from the sync source.
    OpTime _lastFetched;

    // Set by the ReplyMetadataReader upon receiving a new batch.
    BSONObj _metadataObj;

    // Connection to the sync source whose oplog we will be querying. This connection should be
    // created with autoreconnect set to true so that it will automatically reconnect on a
    // connection failure. When the OplogFetcher is shut down, the connection will be interrupted
    // via its shutdownAndDisallowReconnect function.
    std::unique_ptr<DBClientConnection> _conn;

    // The tailable, awaitData, exhaust cursor used to fetch oplog entries from the sync source.
    // When an error is encountered, depending on the result of OplogFetcherRestartDecision's
    // shouldContinue function, a new cursor will be created or the oplog fetcher will shut down.
    std::unique_ptr<DBClientCursor> _cursor;

    // A boolean indicating whether we should error if the sync source is not ahead of our initial
    // last fetched OpTime on the first batch. Most of the time this should be set to true,
    // but there are certain special cases, namely during initial sync, where it's acceptable for
    // our sync source to have no ops newer than _lastFetched.
    bool _requireFresherSyncSource;

    DataReplicatorExternalState* const _dataReplicatorExternalState;
    const EnqueueDocumentsFn _enqueueDocumentsFn;
    const Milliseconds _awaitDataTimeout;
    const int _batchSize;

    // Indicates if we want to skip the first document during oplog fetching or not.
    StartingPoint _startingPoint;

    // Handle to currently scheduled _runQuery task.
    executor::TaskExecutor::CallbackHandle _runQueryHandle;
};

}  // namespace repl
}  // namespace mongo

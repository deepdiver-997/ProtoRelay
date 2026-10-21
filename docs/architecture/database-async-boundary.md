# Database asynchronous execution boundaries

`IDBConnection::async_*` keeps the compatibility contract: its default implementation
executes synchronously and invokes completion inline. Driver overrides may finish on
other threads. Protocol/application state must serialize its continuation explicitly.

`AsyncDBConnectionAdapter` is an opt-in wrapper for a synchronous driver. Work runs on
the injected Worker pool and results return through an injected serial `IExecutor`.
It retains the connection until completion, rejects overlapping async operations with
null/false, and clears its in-flight flag before invoking the completion so chained
transactions can continue. `AsyncDBServiceAdapter` optionally applies it at pool creation.
The caller must retain its `ScopedConnection` through the callback chain and drain work
and completions before stopping either runtime. Cancellation of a blocking query is not
provided by the synchronous driver interface.

MariaDB Connector/C still owns socket reads, writes, close, and `_start`/`_cont` state.
Only socket readiness and deadlines cross `IReadinessWaiter`. `MariaDBService` accepts a
waiter factory and injects one per connection. The application composition root installs
an Asio waiter from its IO runtime; direct construction without a factory explicitly
uses the blocking poll adapter. The MariaDB driver no longer examines
`current_io_context()` and does not include Boost/Asio or call `poll` itself.

The Asio adapter completes once on its injected serial executor, supports cancellation,
and observes a duplicated descriptor. Failed setup, cancellation, and runtime destruction
therefore cannot close Connector/C's own socket. A runtime must remain running until its
pending operations complete. The poll adapter keeps iterative blocking progression and
handles interruption, timeout, and invalid descriptors without callback recursion.

This change does not remove Connector/C's other POSIX assumptions (`dlopen`, `fcntl`).
It makes their readiness runtime replaceable without changing the DB state machine.

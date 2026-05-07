# Architecture Refactoring Roadmap: CameraPipelineManager

## Current State
`CameraPipelineManager` is a "God Class" that currently mixes multiple concerns in one lifecycle:

1. RTSP capture via FFmpeg and native readers (`FFmpegProcess`, `NativeReaderWorker`).
2. Video distribution to viewers (`CameraStreamManager`, `MediaMTXPublisher`).
3. Continuous recording and event clip generation (`ContinuousRecorder`, `BufferPipeline`).
4. AI worker lifecycle and IPC (`ai_worker_v2`, shared memory, ZMQ).
5. Runtime state, watchdog, restart, backup stream, and failure recovery.
6. Per-camera metadata caching and latest-frame storage for APIs.

The code already has strong building blocks in separate folders (`streaming/`, `recording/`, `ipc/`, `events/`), but `CameraPipelineManager` still acts as the coordinator for nearly everything. That creates three concrete risks:

1. A fault in AI orchestration can cascade into media or recording.
2. Restart logic is coupled to frame handling, making race conditions more likely.
3. Unit testing is difficult because one class owns process control, queues, state, and side effects.

## Refactoring Goal
The goal is not to create many modules "for architecture slides". The goal is to split by failure domain and runtime responsibility so that:

1. Media ingest can fail and recover without breaking recording or AI state.
2. Recording backpressure does not block streaming or AI.
3. AI worker crashes do not interrupt raw video capture.
4. Health checks, restart policy, and state transitions become explicit and testable.

## Recommended Target Architecture

Instead of stopping at 3 modules, the practical production-ready split should be 6 core modules and 3 optional modules.

### Core modules

#### 1. `PipelineRegistry`
- **Responsibility:** Own the per-camera runtime object graph and expose lookup by `camera_id`.
- **Owns:** `CameraPipelineHandle`, per-camera config snapshot, current state, references to orchestrators.
- **Why needed:** Today, `pipelines_`, restart counters, backup-state flags, and timestamps are spread across manager internals. Registry centralizes ownership and removes hidden shared state.
- **Notes:** This is a lightweight coordination module, not a worker.

#### 2. `MediaOrchestrator`
- **Responsibility:** Start/stop RTSP ingest and decode, manage primary/sub stream selection, publish raw frame packets to downstream consumers.
- **Owns:** `FFmpegProcess`, `NativeReaderWorker`, reconnect policy for ingest only.
- **Inputs:** Camera RTSP config, resolution/FPS policy, backup stream policy.
- **Outputs:** `FrameEnvelope` objects to a frame bus.
- **Failure boundary:** If FFmpeg dies, only media ingest restarts.

#### 3. `FrameBus`
- **Responsibility:** Fan out frames to multiple consumers without letting one consumer block the others.
- **Owns:** `IFrameConsumer` registration, per-consumer queue policy, drop strategy, optional FPS throttling.
- **Why needed:** This is the missing glue. Without it, orchestration code remains coupled because every consumer is wired manually inside the manager.
- **Minimum interface:**
  - `subscribe(camera_id, consumer)`
  - `unsubscribe(camera_id, consumer_id)`
  - `publish(camera_id, FrameEnvelope)`
- **Recommended payload:** raw frame, JPEG preview, timestamp, frame number, source stream, health flags.

#### 4. `RecordingOrchestrator`
- **Responsibility:** Handle continuous recording and event clip generation.
- **Owns:** `ContinuousRecorder`, `BufferPipeline`, clip trigger workflow.
- **Inputs:** Frames from `FrameBus`, record/clip commands from API and event engine.
- **Outputs:** Segments and clips on disk, recording status.
- **Failure boundary:** Slow disk or clip export failures stay isolated from AI and live view.

#### 5. `StreamingOrchestrator`
- **Responsibility:** Deliver live video and metadata to viewers.
- **Owns:** `CameraStreamManager`, `MediaMTXPublisher`, stream session counters, codec metadata.
- **Inputs:** Frames or encoded packets from `MediaOrchestrator`, AI overlays/metadata from event side.
- **Outputs:** WebSocket/WebRTC/HLS-facing delivery.
- **Why separate:** Viewer demand, codec negotiation, and publishing are operationally different from ingest.

#### 6. `AIWorkerManager`
- **Responsibility:** Manage AI process lifecycle and AI IPC only.
- **Owns:** `SharedMemoryManager`, worker process, ZMQ request/reply or pub/sub bridges, frame sampling policy.
- **Inputs:** Sampled frames from `FrameBus`.
- **Outputs:** Objects, tracks, attributes, metadata, alerts.
- **Failure boundary:** AI worker restarts do not restart video ingest.

### Cross-cutting core support modules

#### 7. `HealthMonitor`
- **Responsibility:** Global watchdog and restart supervisor.
- **Owns:** liveness timers, backoff counters, degraded/failed state transitions, restart budget.
- **Checks:** media ingest heartbeat, AI worker heartbeat, recording health, queue pressure.
- **Why needed:** Restart logic should not live inside the same module that owns the failing process.

#### 8. `PipelineStateStore`
- **Responsibility:** Store the latest observable runtime state for APIs and metrics.
- **Owns:** latest frame thumbnail, latest metadata JSON, current FPS, last frame timestamp, last error, camera state.
- **Why needed:** APIs like `getLatestFrame`, `getLatestMetadata`, and camera stats should read from a dedicated read model, not from live worker internals.

#### 9. `EventDispatcher`
- **Responsibility:** Normalize AI output into internal domain events and forward them to `EventManager`, WebSocket, alerting, and recording triggers.
- **Why needed:** AI output should not directly manipulate recorders or stream managers.
- **Existing integrations to preserve:** `ZmqEventBridge`, `EventManager`, `AttendanceTracker`, `CounterBucketAggregator`.

## Optional modules worth adding only if needed

### `ConfigSnapshotService`
- Freeze per-camera config at pipeline start.
- Prevents reading mutable DB/config state from multiple threads during runtime.
- Useful when cameras support live config updates.

### `BackpressureController`
- Centralizes queue limits, frame dropping rules, AI sampling FPS, and overload behavior.
- Needed if camera count grows or hardware is near saturation.

### `MetricsCollector`
- Emits Prometheus-friendly internal metrics per camera and per module.
- The project already exposes metrics in `http_server.cpp`; this module would make them first-class rather than ad hoc.

## Suggested Dependency Flow

The recommended runtime flow is:

`CameraManager`
-> `PipelineRegistry`
-> `MediaOrchestrator`
-> `FrameBus`
-> `RecordingOrchestrator`
-> `StreamingOrchestrator`
-> `AIWorkerManager`
-> `EventDispatcher`
-> `EventManager` / alerts / attendance / counters

Supporting modules:

`HealthMonitor`
-> observes `MediaOrchestrator`, `RecordingOrchestrator`, `AIWorkerManager`

`PipelineStateStore`
-> read model for HTTP/WebSocket/system metrics

## Minimal Object Model

A good practical split is:

### `CameraPipelineHandle`
- `camera_id`
- `config_snapshot`
- `media`
- `recording`
- `streaming`
- `ai`
- `state_store`

### `FrameEnvelope`
- `camera_id`
- `timestamp_ms`
- `frame_index`
- `cv::Mat raw_frame`
- `std::vector<uchar> jpeg_preview`
- `std::string source_stream`
- `bool is_backup_stream`

### `IFrameConsumer`
- `onFrame(const FrameEnvelope&)`
- `onStreamStateChanged(...)`
- `consumerName()`

This is enough to decouple most of the current `PipelineContext`.

## Mapping Current Code to New Modules

Current responsibilities can be moved incrementally:

1. `PipelineContext` in `src/core/camera_pipeline_manager.cpp`
   -> split into `CameraPipelineHandle` plus per-module private state.
2. `FFmpegProcess`, `NativeReaderWorker`
   -> move under `MediaOrchestrator`.
3. `BufferPipeline`, `ContinuousRecorder`
   -> move under `RecordingOrchestrator`.
4. `MediaMtxPublisher`, `CameraStreamManager`
   -> move under `StreamingOrchestrator`.
5. `SharedMemoryManager`, AI `QProcess`, ZMQ message handling
   -> move under `AIWorkerManager`.
6. `last_frame_ts`, `current_fps`, `last_metadata_json_`, `latest_frame`
   -> move under `PipelineStateStore`.
7. global watchdog timer, restart counters, failed camera tracking
   -> move under `HealthMonitor`.

## Recommended Implementation Phases

### Phase 1: Extract contracts first
- Create `FrameEnvelope`.
- Create `IFrameConsumer`.
- Create `PipelineStateStore`.
- Replace direct mutable access to latest frame/metadata with `PipelineStateStore`.

This phase gives immediate structure without changing behavior much.

### Phase 2: Move ingest into `MediaOrchestrator`
- Move `FFmpegProcess`, `NativeReaderWorker`, and frame callbacks out of `CameraPipelineManager`.
- Let `MediaOrchestrator` publish frames into `FrameBus`.
- Keep recording and AI still connected through compatibility adapters.

### Phase 3: Move recording into `RecordingOrchestrator`
- Move `BufferPipeline` and `ContinuousRecorder` ownership.
- Keep `triggerEventRecording()` as a facade that forwards to `RecordingOrchestrator`.

### Phase 4: Move AI into `AIWorkerManager`
- Move shared memory and worker process lifecycle.
- Move sampling policy and worker restart logic.
- Return AI outputs through `EventDispatcher` instead of writing directly into mixed pipeline state.

### Phase 5: Move restart/watchdog into `HealthMonitor`
- Centralize backoff, restart budget, degraded/failed transitions.
- Remove restart-related mutable maps from `CameraPipelineManager`.

### Phase 6: Shrink `CameraPipelineManager` into a facade
- Final role: route API requests to the right module and coordinate startup/shutdown.
- At this point it becomes a thin composition root rather than an execution-heavy class.

## Which Modules Are Actually Required?

If the question is "do we really need to add more modules?", the short answer is:

### Required now
- `MediaOrchestrator`
- `RecordingOrchestrator`
- `AIWorkerManager`
- `HealthMonitor`
- `FrameBus`
- `PipelineStateStore`

### Nice to have later
- `EventDispatcher`
- `ConfigSnapshotService`
- `BackpressureController`
- `MetricsCollector`

If only 3 modules are created, the design will still improve, but shared mutable state and restart logic will remain tangled. The biggest missing pieces in that smaller design are `FrameBus` and `PipelineStateStore`.

## Practical Recommendation for This Repository

Based on the current codebase, the safest next step is:

1. Keep `CameraManager` as the lifecycle entry point.
2. Reduce `CameraPipelineManager` into a facade gradually, not with a big-bang rewrite.
3. Add `FrameBus`, `PipelineStateStore`, and `HealthMonitor` before splitting everything else.
4. Then extract `MediaOrchestrator` and `RecordingOrchestrator`.
5. Extract `AIWorkerManager` last if the AI path is still changing rapidly.

This order minimizes regression risk because media ingest and state access are the two hottest paths today.

## Conclusion
The detailed production-oriented target is not 3 modules, but effectively 6 core modules plus a few optional support modules. If you want the smallest split that is still technically sound for this repository, use:

1. `MediaOrchestrator`
2. `RecordingOrchestrator`
3. `AIWorkerManager`
4. `FrameBus`
5. `PipelineStateStore`
6. `HealthMonitor`

That gives clear responsibility boundaries, cleaner failure isolation, and a realistic path to refactor the current `CameraPipelineManager` without destabilizing the system.

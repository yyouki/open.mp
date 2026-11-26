//! Windows x86 SA-MP network/download adapter.
//!
//! Native GTA model registration is deliberately not attempted here. This plugin
//! negotiates the extension, downloads verified DFF/TXD assets, and exposes copied
//! model metadata for the next native-rendering milestone.

#[cfg(not(all(windows, target_arch = "x86")))]
compile_error!("custom-vehicle-windows supports only 32-bit Windows x86 targets");

use samp_client_sdk::{
    Samp, SampClientSdkClientVersion, SampClientSdkDirection, SampClientSdkHookAction,
    Subscription,
};
use std::{
    ffi::c_void,
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    path::{Path, PathBuf},
    ptr,
    slice,
    sync::{
        Arc, Condvar, Mutex,
        atomic::{AtomicBool, AtomicPtr, Ordering},
        mpsc::{self, Receiver, RecvTimeoutError, Sender},
    },
    thread::{self, JoinHandle},
    time::Duration,
};
use windows_sys::Win32::{
    Foundation::{HINSTANCE, TRUE},
    System::{
        LibraryLoader::DisableThreadLibraryCalls,
        SystemServices::{DLL_PROCESS_ATTACH, DLL_PROCESS_DETACH},
    },
};
use windows_sys::core::BOOL;

const RPC_MODEL_REQUEST: u8 = 179;
const RPC_REQUEST_DFF: u8 = 181;
const RPC_REQUEST_TXD: u8 = 182;
const RPC_FINISH_DOWNLOAD: u8 = 184;
const RPC_CLIENT_SUPPORTS_VEHICLES: u8 = 186;
const RPC_SERVER_SUPPORTS_VEHICLES: u8 = 187;

const BRIDGE_ABI_VERSION: u32 = 1;
const MAX_RPC_BYTES: usize = 1024 * 1024;
const MAX_ASSET_BYTES: usize = 512 * 1024 * 1024;
const DOWNLOAD_ATTEMPTS: usize = 3;
const RETRY_DELAYS: [Duration; DOWNLOAD_ATTEMPTS] = [
    Duration::from_millis(500),
    Duration::from_secs(1),
    Duration::from_secs(2),
];

#[repr(C)]
struct CustomVehicleAdapter {
    _private: [u8; 0],
}

#[repr(C)]
struct CustomVehicleCallbacksV1 {
    struct_size: u32,
    abi_version: u32,
    user_data: *mut c_void,
    send_rpc: Option<
        unsafe extern "C" fn(
            user_data: *mut c_void,
            rpc_id: u8,
            payload: *const u8,
            payload_size: usize,
        ) -> i32,
    >,
    begin_download: Option<
        unsafe extern "C" fn(
            user_data: *mut c_void,
            asset_type: u8,
            checksum: u32,
            expected_size: u32,
            url: *const u8,
            url_size: usize,
        ) -> i32,
    >,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct CustomVehicleModelRequestV1 {
    pool_id: u32,
    model_count: u32,
    model_type: u8,
    virtual_world: i32,
    base_id: i32,
    new_id: i32,
    dff_checksum: u32,
    txd_checksum: u32,
    dff_size: u32,
    txd_size: u32,
    time_on: u8,
    time_off: u8,
}

unsafe extern "C" {
    fn custom_vehicle_adapter_create(
        callbacks: *const CustomVehicleCallbacksV1,
    ) -> *mut CustomVehicleAdapter;
    fn custom_vehicle_adapter_destroy(adapter: *mut CustomVehicleAdapter);
    fn custom_vehicle_adapter_begin_connection(adapter: *mut CustomVehicleAdapter) -> i32;
    fn custom_vehicle_adapter_end_connection(adapter: *mut CustomVehicleAdapter);
    fn custom_vehicle_adapter_handle_server_rpc(
        adapter: *mut CustomVehicleAdapter,
        rpc_id: u8,
        payload: *const u8,
        payload_size: usize,
    ) -> i32;
    fn custom_vehicle_adapter_complete_asset(
        adapter: *mut CustomVehicleAdapter,
        asset_type: u8,
        checksum: u32,
        data: *const u8,
        data_size: usize,
    ) -> i32;
    fn custom_vehicle_adapter_retry_active_asset(adapter: *mut CustomVehicleAdapter) -> i32;
    fn custom_vehicle_adapter_model_count(adapter: *const CustomVehicleAdapter) -> usize;
    fn custom_vehicle_adapter_copy_model(
        adapter: *const CustomVehicleAdapter,
        index: usize,
        output: *mut CustomVehicleModelRequestV1,
    ) -> i32;
}

struct OutgoingRpc {
    id: u8,
    payload: Vec<u8>,
}

struct DownloadJob {
    asset_type: u8,
    checksum: u32,
    expected_size: u32,
    url: String,
}

enum WorkerMessage<T> {
    Job(T),
    Shutdown,
}

struct BridgeContext {
    adapter: AtomicPtr<CustomVehicleAdapter>,
    hooks_ready: AtomicBool,
    outgoing: Sender<WorkerMessage<OutgoingRpc>>,
    downloads: Sender<WorkerMessage<DownloadJob>>,
    cache_root: PathBuf,
    stopping: AtomicBool,
}

impl BridgeContext {
    fn adapter(&self) -> *mut CustomVehicleAdapter {
        self.adapter.load(Ordering::Acquire)
    }
}

struct PluginState {
    context: Option<Arc<BridgeContext>>,
    subscriptions: Vec<Subscription>,
    workers: Vec<JoinHandle<()>>,
    initializing: bool,
    shutting_down: bool,
}

impl PluginState {
    const fn new() -> Self {
        Self {
            context: None,
            subscriptions: Vec::new(),
            workers: Vec::new(),
            initializing: false,
            shutting_down: false,
        }
    }
}

struct InitializationGuard;

impl Drop for InitializationGuard {
    fn drop(&mut self) {
        STATE
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .initializing = false;
        INITIALIZATION_FINISHED.notify_all();
    }
}

static STATE: Mutex<PluginState> = Mutex::new(PluginState::new());
static INITIALIZATION_FINISHED: Condvar = Condvar::new();

#[unsafe(no_mangle)]
unsafe extern "system" fn DllMain(
    instance: HINSTANCE,
    reason: u32,
    _reserved: *mut c_void,
) -> BOOL {
    match reason {
        DLL_PROCESS_ATTACH => {
            unsafe { DisableThreadLibraryCalls(instance) };
            let mut state = STATE.lock().unwrap_or_else(|error| error.into_inner());
            state.initializing = true;
            state.shutting_down = false;
            drop(state);

            if thread::Builder::new()
                .name("openmp-custom-vehicle-init".into())
                .spawn(initialize)
                .is_err()
            {
                STATE
                    .lock()
                    .unwrap_or_else(|error| error.into_inner())
                    .initializing = false;
                INITIALIZATION_FINISHED.notify_all();
            }
        }
        DLL_PROCESS_DETACH => {}
        _ => {}
    }
    TRUE
}

fn initialize() {
    let _guard = InitializationGuard;

    let samp = match Samp::connect(Duration::from_secs(30)) {
        Ok(samp) => samp,
        Err(_) => return,
    };

    // These builds have independently verified direct network bridges in the pinned ASI host.
    // DL R1 is intentionally gated until native artwork and custom transfers are multiplexed.
    match samp.version() {
        Ok(
            SampClientSdkClientVersion::R1
            | SampClientSdkClientVersion::R3_1
            | SampClientSdkClientVersion::R5_1,
        ) => {}
        _ => return,
    }

    let cache_root = cache_root();
    let (outgoing_tx, outgoing_rx) = mpsc::channel();
    let (download_tx, download_rx) = mpsc::channel();
    let context = Arc::new(BridgeContext {
        adapter: AtomicPtr::new(ptr::null_mut()),
        hooks_ready: AtomicBool::new(false),
        outgoing: outgoing_tx,
        downloads: download_tx,
        cache_root,
        stopping: AtomicBool::new(false),
    });

    let mut workers = Vec::new();

    let outgoing_context = Arc::clone(&context);
    let (outgoing_ready_tx, outgoing_ready_rx) = mpsc::sync_channel(1);
    match thread::Builder::new()
        .name("openmp-custom-vehicle-rpc-send".into())
        .spawn(move || outgoing_worker(outgoing_context, outgoing_rx, outgoing_ready_tx))
    {
        Ok(worker) => workers.push(worker),
        Err(_) => return,
    }

    match outgoing_ready_rx.recv_timeout(Duration::from_secs(35)) {
        Ok(true) => {}
        _ => {
            stop_workers(&context, workers);
            return;
        }
    }

    let download_context = Arc::clone(&context);
    match thread::Builder::new()
        .name("openmp-custom-vehicle-download".into())
        .spawn(move || download_worker(download_context, download_rx))
    {
        Ok(worker) => workers.push(worker),
        Err(_) => {
            stop_workers(&context, workers);
            return;
        }
    }

    let callbacks = CustomVehicleCallbacksV1 {
        struct_size: std::mem::size_of::<CustomVehicleCallbacksV1>() as u32,
        abi_version: BRIDGE_ABI_VERSION,
        user_data: Arc::as_ptr(&context).cast_mut().cast(),
        send_rpc: Some(send_rpc_callback),
        begin_download: Some(begin_download_callback),
    };

    let adapter = unsafe { custom_vehicle_adapter_create(&callbacks) };
    if adapter.is_null() {
        stop_workers(&context, workers);
        return;
    }
    context.adapter.store(adapter, Ordering::Release);

    let callback_context = Arc::clone(&context);
    let subscriptions = match samp.net().on_rpc(
        SampClientSdkDirection::Incoming,
        move |event| handle_server_rpc(&callback_context, event),
    ) {
        Ok(subscription) => vec![subscription],
        Err(_) => {
            cleanup_context(&context, Vec::new(), workers);
            return;
        }
    };

    // The sender worker begins RPC 186 only after the selected client profile reports
    // an initialized connection state and this incoming hook is ready to receive RPC 187.
    context.hooks_ready.store(true, Ordering::Release);

    let mut state = STATE.lock().unwrap_or_else(|error| error.into_inner());
    if state.shutting_down {
        drop(state);
        cleanup_context(&context, subscriptions, workers);
        return;
    }
    state.context = Some(context);
    state.subscriptions = subscriptions;
    state.workers = workers;
}

fn handle_server_rpc(
    context: &BridgeContext,
    event: &mut samp_client_sdk::events::Event<'_>,
) -> SampClientSdkHookAction {
    let rpc_id = event.id();
    let bit_len = event.remaining_bits();

    if bit_len > MAX_RPC_BYTES.saturating_mul(8) || bit_len % 8 != 0 {
        // RPC 179 and 187 are extension-only in the supported non-DL profiles.
        return if rpc_id == RPC_MODEL_REQUEST || rpc_id == RPC_SERVER_SUPPORTS_VEHICLES {
            SampClientSdkHookAction::Block
        } else {
            SampClientSdkHookAction::Continue
        };
    }

    if event.reset_read().is_err() {
        return SampClientSdkHookAction::Continue;
    }

    let payload = match event.read_bits(bit_len) {
        Ok(payload) => payload,
        Err(_) => return SampClientSdkHookAction::Continue,
    };

    let adapter = context.adapter();
    if adapter.is_null() {
        return SampClientSdkHookAction::Continue;
    }

    let payload_ptr = if payload.is_empty() {
        ptr::null()
    } else {
        payload.as_ptr()
    };
    let result = unsafe {
        custom_vehicle_adapter_handle_server_rpc(
            adapter,
            rpc_id,
            payload_ptr,
            payload.len(),
        )
    };

    match result {
        1 => SampClientSdkHookAction::Block,
        0 => {
            // Native RPCs must reach the original parser with their read cursor restored.
            if event.reset_read().is_err() {
                SampClientSdkHookAction::Block
            } else {
                SampClientSdkHookAction::Continue
            }
        }
        _ if rpc_id == RPC_MODEL_REQUEST || rpc_id == RPC_SERVER_SUPPORTS_VEHICLES => {
            SampClientSdkHookAction::Block
        }
        _ => {
            if event.reset_read().is_err() {
                SampClientSdkHookAction::Block
            } else {
                SampClientSdkHookAction::Continue
            }
        }
    }
}

unsafe extern "C" fn send_rpc_callback(
    user_data: *mut c_void,
    rpc_id: u8,
    payload: *const u8,
    payload_size: usize,
) -> i32 {
    std::panic::catch_unwind(|| {
        if user_data.is_null()
            || (payload.is_null() && payload_size != 0)
            || payload_size > MAX_RPC_BYTES
        {
            return 0;
        }

        let context = unsafe { &*user_data.cast::<BridgeContext>() };
        if context.stopping.load(Ordering::Acquire) {
            return 0;
        }

        let bytes = if payload_size == 0 {
            Vec::new()
        } else {
            unsafe { slice::from_raw_parts(payload, payload_size) }.to_vec()
        };
        if context
            .outgoing
            .send(WorkerMessage::Job(OutgoingRpc {
                id: rpc_id,
                payload: bytes,
            }))
            .is_ok()
        {
            1
        } else {
            0
        }
    })
    .unwrap_or(0)
}

unsafe extern "C" fn begin_download_callback(
    user_data: *mut c_void,
    asset_type: u8,
    checksum: u32,
    expected_size: u32,
    url: *const u8,
    url_size: usize,
) -> i32 {
    std::panic::catch_unwind(|| {
        if user_data.is_null()
            || url.is_null()
            || url_size == 0
            || url_size > 255
            || expected_size == 0
            || expected_size as usize > MAX_ASSET_BYTES
            || (asset_type != 1 && asset_type != 2)
        {
            return 0;
        }

        let context = unsafe { &*user_data.cast::<BridgeContext>() };
        if context.stopping.load(Ordering::Acquire) {
            return 0;
        }

        let url_bytes = unsafe { slice::from_raw_parts(url, url_size) };
        let Ok(url) = std::str::from_utf8(url_bytes) else {
            return 0;
        };

        if context
            .downloads
            .send(WorkerMessage::Job(DownloadJob {
                asset_type,
                checksum,
                expected_size,
                url: url.to_owned(),
            }))
            .is_ok()
        {
            1
        } else {
            0
        }
    })
    .unwrap_or(0)
}

fn outgoing_worker(
    context: Arc<BridgeContext>,
    receiver: Receiver<WorkerMessage<OutgoingRpc>>,
    ready: mpsc::SyncSender<bool>,
) {
    let samp = match Samp::connect(Duration::from_secs(30)) {
        Ok(samp) => samp,
        Err(_) => {
            let _ = ready.send(false);
            return;
        }
    };

    let version = match samp.version() {
        Ok(
            version @ (
                SampClientSdkClientVersion::R1
                | SampClientSdkClientVersion::R3_1
                | SampClientSdkClientVersion::R5_1
            ),
        ) => version,
        _ => {
            let _ = ready.send(false);
            return;
        }
    };
    let _ = ready.send(true);

    let net = samp.net();
    let mut session_active = false;

    loop {
        match receiver.recv_timeout(Duration::from_millis(100)) {
            Ok(WorkerMessage::Shutdown) | Err(RecvTimeoutError::Disconnected) => break,
            Ok(WorkerMessage::Job(request)) => {
                if context.stopping.load(Ordering::Acquire) {
                    break;
                }

                let bit_len = request.payload.len().saturating_mul(8);
                if bit_len <= MAX_RPC_BYTES * 8 {
                    let sent = net
                        .send_rpc(request.id, &request.payload, bit_len)
                        .and_then(|receipt| receipt.wait(Duration::from_secs(10)));
                    if sent.is_err() {
                        let adapter = context.adapter();
                        if !adapter.is_null() {
                            match request.id {
                                RPC_REQUEST_DFF | RPC_REQUEST_TXD | RPC_FINISH_DOWNLOAD => unsafe {
                                    custom_vehicle_adapter_retry_active_asset(adapter);
                                },
                                RPC_CLIENT_SUPPORTS_VEHICLES => {
                                    // A failed capability send leaves the portable session pending.
                                    unsafe { custom_vehicle_adapter_end_connection(adapter) };
                                    session_active = false;
                                }
                                _ => {}
                            }
                        }
                    }
                }
            }
            Err(RecvTimeoutError::Timeout) => {}
        }

        if context.stopping.load(Ordering::Acquire) || !context.hooks_ready.load(Ordering::Acquire) {
            continue;
        }

        // Verified host profiles report R1=14, R3-1/R5-1=15 during AwaitingJoin,
        // and all three report 14 once connected. No guessed offsets are used here.
        let game_state = samp.game_state().ok();
        let connected = match version {
            SampClientSdkClientVersion::R1 => game_state == Some(14),
            SampClientSdkClientVersion::R3_1 | SampClientSdkClientVersion::R5_1 => {
                matches!(game_state, Some(14 | 15))
            }
            _ => false,
        };

        let adapter = context.adapter();
        if adapter.is_null() {
            continue;
        }

        if connected && !session_active {
            if unsafe { custom_vehicle_adapter_begin_connection(adapter) } != 0 {
                session_active = true;
            }
        } else if !connected && session_active {
            unsafe { custom_vehicle_adapter_end_connection(adapter) };
            session_active = false;
        }
    }

    let adapter = context.adapter();
    if !adapter.is_null() && session_active {
        unsafe { custom_vehicle_adapter_end_connection(adapter) };
    }
}

fn download_worker(
    context: Arc<BridgeContext>,
    receiver: Receiver<WorkerMessage<DownloadJob>>,
) {
    let agent = ureq::AgentBuilder::new()
        .timeout(Duration::from_secs(30))
        .build();

    while let Ok(message) = receiver.recv() {
        match message {
            WorkerMessage::Shutdown => break,
            WorkerMessage::Job(job) => {
                if context.stopping.load(Ordering::Acquire) {
                    break;
                }

                let mut completed = false;
                for attempt in 0..DOWNLOAD_ATTEMPTS {
                    match fetch_asset(&agent, &context.cache_root, &job) {
                        Ok(bytes) => {
                            let adapter = context.adapter();
                            if !adapter.is_null()
                                && unsafe {
                                    custom_vehicle_adapter_complete_asset(
                                        adapter,
                                        job.asset_type,
                                        job.checksum,
                                        bytes.as_ptr(),
                                        bytes.len(),
                                    )
                                } != 0
                            {
                                completed = true;
                                break;
                            }
                        }
                        Err(_) => {}
                    }

                    if context.stopping.load(Ordering::Acquire) {
                        break;
                    }
                    thread::sleep(RETRY_DELAYS[attempt]);
                }

                if !completed && !context.stopping.load(Ordering::Acquire) {
                    let adapter = context.adapter();
                    if !adapter.is_null() {
                        unsafe { custom_vehicle_adapter_retry_active_asset(adapter) };
                    }
                }
            }
        }
    }
}

fn fetch_asset(
    agent: &ureq::Agent,
    cache_root: &Path,
    job: &DownloadJob,
) -> Result<Vec<u8>, String> {
    if job.expected_size == 0 || job.expected_size as usize > MAX_ASSET_BYTES {
        return Err("asset size exceeds the configured safety limit".into());
    }

    if let Some(cached) = read_verified_cache(cache_root, job) {
        return Ok(cached);
    }

    let response = agent
        .get(&job.url)
        .call()
        .map_err(|error| error.to_string())?;

    if let Some(content_length) = response.header("Content-Length") {
        let parsed = content_length
            .parse::<u64>()
            .map_err(|_| "invalid Content-Length".to_owned())?;
        if parsed != u64::from(job.expected_size) {
            return Err("HTTP Content-Length does not match the model manifest".into());
        }
    }

    let mut bytes = Vec::with_capacity(job.expected_size as usize);
    response
        .into_reader()
        .take(u64::from(job.expected_size) + 1)
        .read_to_end(&mut bytes)
        .map_err(|error| error.to_string())?;

    if bytes.len() != job.expected_size as usize || crc32(&bytes) != job.checksum {
        return Err("downloaded asset failed exact size or CRC32 verification".into());
    }

    store_verified_cache(cache_root, job, &bytes)
        .map_err(|error| format!("unable to store verified asset: {error}"))?;
    Ok(bytes)
}

fn cache_path(root: &Path, job: &DownloadJob) -> PathBuf {
    let (prefix, extension) = if job.asset_type == 1 {
        ("dff-", ".dff")
    } else {
        ("txd-", ".txd")
    };
    root.join(format!("{prefix}{:08x}{extension}", job.checksum))
}

fn read_verified_cache(root: &Path, job: &DownloadJob) -> Option<Vec<u8>> {
    let path = cache_path(root, job);
    let mut file = File::open(path).ok()?;
    if file.metadata().ok()?.len() != u64::from(job.expected_size) {
        return None;
    }

    let mut bytes = Vec::with_capacity(job.expected_size as usize);
    file.read_to_end(&mut bytes).ok()?;
    (bytes.len() == job.expected_size as usize && crc32(&bytes) == job.checksum)
        .then_some(bytes)
}

fn store_verified_cache(root: &Path, job: &DownloadJob, bytes: &[u8]) -> std::io::Result<()> {
    if bytes.len() != job.expected_size as usize || crc32(bytes) != job.checksum {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "asset checksum or size mismatch",
        ));
    }

    fs::create_dir_all(root)?;
    let destination = cache_path(root, job);

    if read_verified_cache(root, job).is_some() {
        return Ok(());
    }

    if destination.exists() {
        fs::remove_file(&destination)?;
    }

    let temporary = destination.with_extension(match job.asset_type {
        1 => "dff.part",
        _ => "txd.part",
    });
    if temporary.exists() {
        fs::remove_file(&temporary)?;
    }

    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temporary)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    drop(file);

    let mut persisted = Vec::new();
    File::open(&temporary)?.read_to_end(&mut persisted)?;
    if persisted.len() != job.expected_size as usize || crc32(&persisted) != job.checksum {
        fs::remove_file(&temporary)?;
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "persisted cache bytes failed verification",
        ));
    }

    fs::rename(&temporary, &destination)?;
    Ok(())
}

fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            let mask = 0u32.wrapping_sub(crc & 1);
            crc = (crc >> 1) ^ (0xEDB8_8320 & mask);
        }
    }
    !crc
}

fn cache_root() -> PathBuf {
    let base = std::env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .or_else(|| std::env::current_dir().ok())
        .unwrap_or_else(std::env::temp_dir);
    base.join("open.mp").join("custom-vehicle")
}

fn stop_workers(
    context: &BridgeContext,
    workers: Vec<JoinHandle<()>>,
) {
    context.stopping.store(true, Ordering::Release);
    let _ = context.outgoing.send(WorkerMessage::Shutdown);
    let _ = context.downloads.send(WorkerMessage::Shutdown);
    for worker in workers {
        let _ = worker.join();
    }

    let adapter = context.adapter.swap(ptr::null_mut(), Ordering::AcqRel);
    if !adapter.is_null() {
        unsafe {
            custom_vehicle_adapter_end_connection(adapter);
            custom_vehicle_adapter_destroy(adapter);
        }
    }
}

fn drain_subscriptions(
    subscriptions: Vec<Subscription>,
) -> Result<(), Vec<Subscription>> {
    let mut undrained = Vec::new();
    for subscription in subscriptions {
        if let Err(error) = subscription.unregister_and_wait() {
            // The SDK returns ownership when synchronized removal fails; retain it for retry.
            undrained.push(error.into_subscription());
        }
    }

    if undrained.is_empty() {
        Ok(())
    } else {
        Err(undrained)
    }
}

fn cleanup_context(
    context: &Arc<BridgeContext>,
    subscriptions: Vec<Subscription>,
    workers: Vec<JoinHandle<()>>,
) {
    if let Err(subscriptions) = drain_subscriptions(subscriptions) {
        // Retain every callback dependency until a later shutdown can drain active hooks.
        let mut state = STATE.lock().unwrap_or_else(|error| error.into_inner());
        state.context = Some(Arc::clone(context));
        state.subscriptions = subscriptions;
        state.workers = workers;
        return;
    }

    stop_workers(context, workers);
}

#[unsafe(no_mangle)]
pub extern "system" fn CustomVehicleWindows_Shutdown() -> BOOL {
    let (context, mut subscriptions, workers) = {
        let mut state = STATE.lock().unwrap_or_else(|error| error.into_inner());
        state.shutting_down = true;
        while state.initializing {
            state = INITIALIZATION_FINISHED
                .wait(state)
                .unwrap_or_else(|error| error.into_inner());
        }
        (
            state.context.take(),
            std::mem::take(&mut state.subscriptions),
            std::mem::take(&mut state.workers),
        )
    };

    let Some(context) = context else {
        return TRUE;
    };

    if let Err(subscriptions) = drain_subscriptions(subscriptions) {
        // Keep the adapter, callback context, and workers alive if any hook may still be active.
        let mut state = STATE.lock().unwrap_or_else(|error| error.into_inner());
        state.context = Some(context);
        state.subscriptions = subscriptions;
        state.workers = workers;
        return 0;
    }

    stop_workers(&context, workers);
    TRUE
}

#[unsafe(no_mangle)]
pub extern "system" fn CustomVehicleWindows_ModelCount() -> usize {
    let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
    let Some(context) = state.context.as_ref() else {
        return 0;
    };
    let adapter = context.adapter();
    if adapter.is_null() {
        return 0;
    }
    unsafe { custom_vehicle_adapter_model_count(adapter) }
}

#[unsafe(no_mangle)]
pub extern "system" fn CustomVehicleWindows_CopyModel(
    index: usize,
    output: *mut CustomVehicleModelRequestV1,
) -> i32 {
    if output.is_null() {
        return 0;
    }

    let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
    let Some(context) = state.context.as_ref() else {
        return 0;
    };
    let adapter = context.adapter();
    if adapter.is_null() {
        return 0;
    }
    unsafe { custom_vehicle_adapter_copy_model(adapter, index, output) }
}


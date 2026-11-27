//! Windows x86 SA-MP network/download adapter.
//!
//! Native GTA model registration and vehicle mutation remain disabled. When the
//! exact GTA profile is available, this plugin registers an observation-only game
//! tick, negotiates the extension, downloads verified DFF/TXD assets, and exposes
//! copied model metadata for the next native-rendering milestone.

#[cfg(not(all(windows, target_arch = "x86")))]
compile_error!("custom-vehicle-windows supports only 32-bit Windows x86 targets");

use samp_client_sdk::{
    Samp, SampClientSdkClientVersion, SampClientSdkDirection, SampClientSdkHookAction,
    Subscription,
};
use modkit_abi::{GameContextTokenV1, SubscriptionId, GTA_POOL_VEHICLE_V1};
use modkit_sdk::{Core, GameContext, GtaSaService, Host, SampPoolService, ServiceError};
use std::{
    ffi::c_void,
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    os::windows::ffi::OsStrExt,
    path::{Path, PathBuf},
    ptr,
    slice,
    sync::{
        Arc, Condvar, Mutex, Weak,
        atomic::{AtomicBool, AtomicI32, AtomicPtr, AtomicU32, AtomicUsize, Ordering},
        mpsc::{self, Receiver, RecvTimeoutError, Sender},
    },
    thread::{self, JoinHandle},
    time::Duration,
};
use windows_sys::Win32::{
    Foundation::{HINSTANCE, TRUE},
    System::{
        LibraryLoader::{DisableThreadLibraryCalls, GetModuleHandleA, LoadLibraryW},
        SystemServices::{DLL_PROCESS_ATTACH, DLL_PROCESS_DETACH},
    },
};

// The SDK host is embedded into this ASI and extracted to the user's app-data cache.
// Its module name must remain samp_client_sdk.asi because the pinned SDK resolver uses it.
static EMBEDDED_SDK_HOST: &[u8] = include_bytes!(env!("CUSTOM_VEHICLE_HOST_DLL_PATH"));
static SDK_HOST_LOAD_LOCK: Mutex<()> = Mutex::new(());
static SDK_HOST_MODULE: AtomicPtr<c_void> = AtomicPtr::new(ptr::null_mut());
use windows_sys::core::BOOL;

const RPC_REQUEST_DFF: u8 = 223;
const RPC_REQUEST_TXD: u8 = 224;
const RPC_FINISH_DOWNLOAD: u8 = 226;
const RPC_WORLD_VEHICLE_ADD: u8 = 164;
const RPC_WORLD_VEHICLE_REMOVE: u8 = 165;
const RPC_CLIENT_SUPPORTS_VEHICLES: u8 = 221;
const RPC_INIT_GAME: u8 = 139;
const STREAM_IN_VEHICLE_BITS: usize = 63 * 8;
const MIN_CUSTOM_VEHICLE_MODEL_ID: i32 = 30_001;
const MAX_CUSTOM_VEHICLE_MODEL_ID: i32 = 32_767;

// SA-MP's RakNet fork packet identifiers (from its bundled PacketEnumerations.h).
const PACKET_CONNECTION_ATTEMPT_FAILED: u8 = 29;
const PACKET_NO_FREE_INCOMING_CONNECTIONS: u8 = 31;
const PACKET_DISCONNECTION_NOTIFICATION: u8 = 32;
const PACKET_CONNECTION_LOST: u8 = 33;
const PACKET_CONNECTION_BANNED: u8 = 36;
const PACKET_INVALID_PASSWORD: u8 = 37;

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
struct CustomVehiclePendingVehicleQueue {
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

/// Stable C-compatible copy of a registered custom vehicle model manifest record.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct CustomVehicleModelRequestV1 {
    pub pool_id: u32,
    pub model_count: u32,
    pub model_type: u8,
    pub virtual_world: i32,
    pub base_id: i32,
    pub new_id: i32,
    pub dff_checksum: u32,
    pub txd_checksum: u32,
    pub dff_size: u32,
    pub txd_size: u32,
    pub time_on: u8,
    pub time_off: u8,
}

const _: [(); 44] = [(); std::mem::size_of::<CustomVehicleModelRequestV1>()];
const _: [(); 8] = [(); std::mem::offset_of!(CustomVehicleModelRequestV1, model_type)];
const _: [(); 12] = [(); std::mem::offset_of!(CustomVehicleModelRequestV1, virtual_world)];
const _: [(); 24] = [(); std::mem::offset_of!(CustomVehicleModelRequestV1, dff_checksum)];
const _: [(); 40] = [(); std::mem::offset_of!(CustomVehicleModelRequestV1, time_on)];

/// C-compatible decoded payload for server RPC 164.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct CustomVehicleStreamInVehicleV1 {
    pub vehicle_id: u16,
    pub custom_model_id: i32,
    pub base_model_id: i32,
    pub position: [f32; 3],
    pub angle: f32,
    pub colour1: u8,
    pub colour2: u8,
    pub health: f32,
    pub interior: u8,
    pub door_damage: u32,
    pub panel_damage: u32,
    pub light_damage: u8,
    pub tyre_damage: u8,
    pub siren: u8,
    pub mods: [u8; 14],
    pub paintjob: u8,
    pub body_colour1: i32,
    pub body_colour2: i32,
}

// Keep this ABI in lock-step with custom_vehicle_stream_in_vehicle_v1 in bridge.h.
const _: [(); 76] = [(); std::mem::size_of::<CustomVehicleStreamInVehicleV1>()];
const _: [(); 12] = [(); std::mem::offset_of!(CustomVehicleStreamInVehicleV1, position)];
const _: [(); 68] = [(); std::mem::offset_of!(CustomVehicleStreamInVehicleV1, body_colour1)];

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
    fn custom_vehicle_adapter_is_model_ready(
        adapter: *const CustomVehicleAdapter,
        custom_model_id: i32,
    ) -> i32;
    fn custom_vehicle_adapter_decode_stream_in_vehicle(
        adapter: *const CustomVehicleAdapter,
        payload: *const u8,
        payload_size: usize,
        output: *mut CustomVehicleStreamInVehicleV1,
    ) -> i32;
    fn custom_vehicle_adapter_rewrite_stream_in_vehicle(
        adapter: *const CustomVehicleAdapter,
        payload: *const u8,
        payload_size: usize,
        output: *mut u8,
        output_capacity: usize,
        output_size: *mut usize,
        custom_model_id: *mut i32,
        base_model_id: *mut i32,
    ) -> i32;

    fn custom_vehicle_pending_vehicle_queue_create() -> *mut CustomVehiclePendingVehicleQueue;
    fn custom_vehicle_pending_vehicle_queue_destroy(
        queue: *mut CustomVehiclePendingVehicleQueue,
    ) -> i32;
    fn custom_vehicle_pending_vehicle_queue_size(
        queue: *const CustomVehiclePendingVehicleQueue,
    ) -> usize;
    fn custom_vehicle_pending_vehicle_queue_peek(
        queue: *const CustomVehiclePendingVehicleQueue,
        output: *mut CustomVehicleStreamInVehicleV1,
    ) -> i32;
    fn custom_vehicle_pending_vehicle_queue_enqueue(
        queue: *mut CustomVehiclePendingVehicleQueue,
        request: *const CustomVehicleStreamInVehicleV1,
    ) -> i32;
    fn custom_vehicle_pending_vehicle_queue_pop(
        queue: *mut CustomVehiclePendingVehicleQueue,
        output: *mut CustomVehicleStreamInVehicleV1,
    ) -> i32;
    fn custom_vehicle_pending_vehicle_queue_cancel(
        queue: *mut CustomVehiclePendingVehicleQueue,
        vehicle_id: u16,
    ) -> i32;
    fn custom_vehicle_pending_vehicle_queue_clear(
        queue: *mut CustomVehiclePendingVehicleQueue,
    );
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

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ConnectionEvent {
    GameInitialized,
    Disconnected,
}

fn supports_network_version(version: SampClientSdkClientVersion) -> bool {
    matches!(
        version,
        SampClientSdkClientVersion::R1
            | SampClientSdkClientVersion::R2
            | SampClientSdkClientVersion::R3_1
            | SampClientSdkClientVersion::R4_2
            | SampClientSdkClientVersion::R5_1
            | SampClientSdkClientVersion::Dl
    )
}

fn has_native_game_state(version: SampClientSdkClientVersion) -> bool {
    matches!(
        version,
        SampClientSdkClientVersion::R1
            | SampClientSdkClientVersion::R3_1
            | SampClientSdkClientVersion::R5_1
            | SampClientSdkClientVersion::Dl
    )
}

fn is_connection_termination_packet(packet_id: u8) -> bool {
    matches!(
        packet_id,
        PACKET_CONNECTION_ATTEMPT_FAILED
            | PACKET_NO_FREE_INCOMING_CONNECTIONS
            | PACKET_DISCONNECTION_NOTIFICATION
            | PACKET_CONNECTION_LOST
            | PACKET_CONNECTION_BANNED
            | PACKET_INVALID_PASSWORD
    )
}

struct BridgeContext {
    adapter: AtomicPtr<CustomVehicleAdapter>,
    pending_vehicles: AtomicPtr<CustomVehiclePendingVehicleQueue>,
    hooks_ready: AtomicBool,
    outgoing: Sender<WorkerMessage<OutgoingRpc>>,
    downloads: Sender<WorkerMessage<DownloadJob>>,
    connection_events: Sender<ConnectionEvent>,
    cache_root: PathBuf,
    native_tick_registered: AtomicBool,
    game_thread_tick_count: AtomicU32,
    pending_vehicle_count_on_game_thread: AtomicUsize,
    pending_vehicle_base_exists_on_game_thread: AtomicBool,
    pending_vehicle_id_on_game_thread: AtomicU32,
    pending_vehicle_gta_handle_on_game_thread: AtomicI32,
    stopping: AtomicBool,
}

impl BridgeContext {
    fn adapter(&self) -> *mut CustomVehicleAdapter {
        self.adapter.load(Ordering::Acquire)
    }

    fn pending_vehicle_queue(&self) -> *mut CustomVehiclePendingVehicleQueue {
        self.pending_vehicles.load(Ordering::Acquire)
    }

    fn clear_pending_vehicle_probe(&self) {
        self.pending_vehicle_base_exists_on_game_thread
            .store(false, Ordering::Release);
        self.pending_vehicle_id_on_game_thread
            .store(0, Ordering::Release);
        self.pending_vehicle_gta_handle_on_game_thread
            .store(-1, Ordering::Release);
    }

    fn clear_pending_vehicles(&self) {
        let queue = self.pending_vehicle_queue();
        if !queue.is_null() {
            unsafe { custom_vehicle_pending_vehicle_queue_clear(queue) };
        }
        self.pending_vehicle_count_on_game_thread
            .store(0, Ordering::Release);
        self.clear_pending_vehicle_probe();
    }
}

struct NativeTickCallbackContext {
    context: Weak<BridgeContext>,
    gta: GtaSaService,
    samp_pools: Option<SampPoolService>,
}

struct NativeTickRegistration {
    core: Core,
    subscription: SubscriptionId,
}

struct PluginState {
    context: Option<Arc<BridgeContext>>,
    subscriptions: Vec<Subscription>,
    workers: Vec<JoinHandle<()>>,
    native_tick: Option<NativeTickRegistration>,
    initializing: bool,
    shutting_down: bool,
}

impl PluginState {
    const fn new() -> Self {
        Self {
            context: None,
            subscriptions: Vec::new(),
            workers: Vec::new(),
            native_tick: None,
            initializing: false,
            shutting_down: false,
        }
    }
}

fn register_native_game_tick(context: &Arc<BridgeContext>) -> Option<NativeTickRegistration> {
    // The embedded SDK host is already loaded by initialize(); this newer facade
    // exposes the host-issued, callback-scoped GTA game-thread execution token.
    let host = Host::connect(Duration::from_secs(5)).ok()?;
    let core = host.core().ok()?;
    let mut gta = None;
    for attempt in 0..15 {
        match host.gta_sa_service() {
            Ok(service) => {
                gta = Some(service);
                break;
            }
            Err(ServiceError::NotReady) if attempt < 14 => {
                thread::sleep(Duration::from_millis(100));
            }
            Err(_) => return None,
        }
    }
    let gta = gta?;
    // Pool mappings are game-thread-refreshed owned state. They are optional if
    // the host does not publish the matching SA-MP pool service.
    let samp_pools = host.samp_pools().ok();

    // The host owns this callback context after successful registration and
    // calls the release callback only after in-flight tick callbacks drain.
    let callback_context = NativeTickCallbackContext {
        context: Arc::downgrade(context),
        gta,
        samp_pools,
    };
    let user_data = Box::into_raw(Box::new(callback_context)).cast::<c_void>();
    match unsafe {
        gta.register_tick(
            native_game_tick_callback,
            user_data,
            native_game_tick_release_callback,
        )
    } {
        Ok(subscription) => {
            context.native_tick_registered.store(true, Ordering::Release);
            Some(NativeTickRegistration { core, subscription })
        }
        Err(_) => {
            // Registration failed, so ownership never transferred to the host.
            unsafe { drop(Box::from_raw(user_data.cast::<NativeTickCallbackContext>())); }
            None
        }
    }
}

unsafe extern "system" fn native_game_tick_callback(
    user_data: *mut c_void,
    token: GameContextTokenV1,
) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if user_data.is_null() || !token.is_valid() {
            return;
        }

        // Keep the native execution proof local to this callback. No token or
        // game-thread-only value is retained by the worker threads.
        let _game_context = unsafe { GameContext::from_raw(token) };
        let callback_context = unsafe { &*user_data.cast::<NativeTickCallbackContext>() };
        let Some(context) = callback_context.context.upgrade() else {
            return;
        };
        if context.stopping.load(Ordering::Acquire) {
            return;
        }

        context.game_thread_tick_count.fetch_add(1, Ordering::Relaxed);
        let queue = context.pending_vehicle_queue();
        if queue.is_null() {
            context
                .pending_vehicle_count_on_game_thread
                .store(0, Ordering::Release);
            context.clear_pending_vehicle_probe();
            return;
        }

        let pending = unsafe { custom_vehicle_pending_vehicle_queue_size(queue) };
        context
            .pending_vehicle_count_on_game_thread
            .store(pending, Ordering::Release);

        // Peek the oldest stream-in without removing it. Map the SA-MP vehicle ID
        // to its current GTA handle, then verify it in this scoped game-thread call.
        let mut request = CustomVehicleStreamInVehicleV1::default();
        if unsafe { custom_vehicle_pending_vehicle_queue_peek(queue, &mut request) } != 1 {
            context.clear_pending_vehicle_probe();
            return;
        }

        let Some(pool_service) = callback_context.samp_pools else {
            context.clear_pending_vehicle_probe();
            return;
        };
        let handle = match pool_service.vehicle_handle(request.vehicle_id) {
            Ok(Some(handle)) => handle,
            _ => {
                context.clear_pending_vehicle_probe();
                return;
            }
        };
        let exists = callback_context
            .gta
            .entity_exists(&_game_context, GTA_POOL_VEHICLE_V1, handle)
            .unwrap_or(false);
        if !exists {
            context.clear_pending_vehicle_probe();
            return;
        }

        context
            .pending_vehicle_base_exists_on_game_thread
            .store(true, Ordering::Release);
        context
            .pending_vehicle_id_on_game_thread
            .store(u32::from(request.vehicle_id), Ordering::Release);
        context
            .pending_vehicle_gta_handle_on_game_thread
            .store(handle, Ordering::Release);
    }));
}

unsafe extern "system" fn native_game_tick_release_callback(user_data: *mut c_void) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if user_data.is_null() {
            return;
        }

        let callback_context = unsafe { Box::from_raw(user_data.cast::<NativeTickCallbackContext>()) };
        if let Some(context) = callback_context.context.upgrade() {
            context.native_tick_registered.store(false, Ordering::Release);
            context.clear_pending_vehicle_probe();
        }
        // Dropping the Box releases the callback-owned weak reference.
    }));
}

fn drain_native_game_tick(
    registration: Option<NativeTickRegistration>,
) -> Result<(), NativeTickRegistration> {
    let Some(registration) = registration else {
        return Ok(());
    };

    match registration
        .core
        .unregister_and_wait(registration.subscription, Duration::from_secs(5))
    {
        Ok(()) => Ok(()),
        Err(_) => Err(registration),
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

    // Load the embedded SDK host from a worker thread, never while Windows holds
    // the loader lock inside this plugin's DllMain.
    if !ensure_sdk_host_loaded() {
        return;
    }

    let samp = match Samp::connect(Duration::from_secs(30)) {
        Ok(samp) => samp,
        Err(_) => return,
    };

    // All six recognized SA-MP profiles expose RakClient packet/RPC hooks.
    // R2 and R4-2 use InitGame RPC observation because they lack direct game-state layouts.
    // Custom vehicles use RPC 220-227, leaving native DL artwork traffic untouched.
    let version = match samp.version() {
        Ok(version) if supports_network_version(version) => version,
        _ => return,
    };

    let pending_vehicles = unsafe { custom_vehicle_pending_vehicle_queue_create() };
    if pending_vehicles.is_null() {
        return;
    }

    let cache_root = cache_root();
    let (outgoing_tx, outgoing_rx) = mpsc::channel();
    let (download_tx, download_rx) = mpsc::channel();
    let (connection_events_tx, connection_events_rx) = mpsc::channel();
    let context = Arc::new(BridgeContext {
        adapter: AtomicPtr::new(ptr::null_mut()),
        pending_vehicles: AtomicPtr::new(pending_vehicles),
        hooks_ready: AtomicBool::new(false),
        outgoing: outgoing_tx,
        downloads: download_tx,
        connection_events: connection_events_tx,
        cache_root,
        native_tick_registered: AtomicBool::new(false),
        game_thread_tick_count: AtomicU32::new(0),
        pending_vehicle_count_on_game_thread: AtomicUsize::new(0),
        pending_vehicle_base_exists_on_game_thread: AtomicBool::new(false),
        pending_vehicle_id_on_game_thread: AtomicU32::new(0),
        pending_vehicle_gta_handle_on_game_thread: AtomicI32::new(-1),
        stopping: AtomicBool::new(false),
    });

    let mut workers = Vec::new();

    let outgoing_context = Arc::clone(&context);
    let (outgoing_ready_tx, outgoing_ready_rx) = mpsc::sync_channel(1);
    match thread::Builder::new()
        .name("openmp-custom-vehicle-rpc-send".into())
        .spawn(move || {
            outgoing_worker(
                outgoing_context,
                outgoing_rx,
                connection_events_rx,
                version,
                outgoing_ready_tx,
            )
        })
    {
        Ok(worker) => workers.push(worker),
        Err(_) => {
            stop_workers(&context, workers);
            return;
        }
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

    let rpc_context = Arc::clone(&context);
    let rpc_subscription = match samp.net().on_rpc(
        SampClientSdkDirection::Incoming,
        move |event| handle_server_rpc(&rpc_context, event),
    ) {
        Ok(subscription) => subscription,
        Err(_) => {
            cleanup_context(&context, Vec::new(), workers, None);
            return;
        }
    };

    let packet_context = Arc::clone(&context);
    let packet_subscription = match samp.net().on_packet(
        SampClientSdkDirection::Incoming,
        move |event| handle_server_packet(&packet_context, event),
    ) {
        Ok(subscription) => subscription,
        Err(_) => {
            cleanup_context(&context, vec![rpc_subscription], workers, None);
            return;
        }
    };
    let subscriptions = vec![rpc_subscription, packet_subscription];
    // Native tick registration is optional: unsupported GTA executables keep the
    // network/download path working, but never enable unverified native access.
    let native_tick = register_native_game_tick(&context);

    // The sender waits for InitGame (or a native game-state fallback on full profiles)
    // and both incoming hooks are installed before capability negotiation can begin.
    context.hooks_ready.store(true, Ordering::Release);

    let mut state = STATE.lock().unwrap_or_else(|error| error.into_inner());
    if state.shutting_down {
        drop(state);
        cleanup_context(&context, subscriptions, workers, native_tick);
        return;
    }
    state.context = Some(context);
    state.subscriptions = subscriptions;
    state.workers = workers;
    state.native_tick = native_tick;
}

fn is_custom_vehicle_rpc(rpc_id: u8) -> bool {
    matches!(rpc_id, 220 | 222 | 225 | 227)
}

fn handle_server_rpc(
    context: &BridgeContext,
    event: &mut samp_client_sdk::events::Event<'_>,
) -> SampClientSdkHookAction {
    let rpc_id = event.id();

    // RPC 139 (InitGame) is sent after the server has completed the SA-MP join handshake.
    // It is a lifecycle signal only; the original client still receives and parses it.
    if rpc_id == RPC_INIT_GAME {
        let _ = context.connection_events.send(ConnectionEvent::GameInitialized);
    }

    // Preserve native SA-MP and 0.3.DL RPCs exactly as received. Do not copy or
    // consume their bitstreams; the original game parser owns those packets.
    if rpc_id == RPC_WORLD_VEHICLE_ADD {
        return handle_world_vehicle_add(context, event);
    }
    if rpc_id == RPC_WORLD_VEHICLE_REMOVE {
        return handle_world_vehicle_remove(context, event);
    }

    if !is_custom_vehicle_rpc(rpc_id) {
        return SampClientSdkHookAction::Continue;
    }

    let bit_len = event.remaining_bits();
    if bit_len > MAX_RPC_BYTES.saturating_mul(8) || bit_len % 8 != 0 {
        return SampClientSdkHookAction::Continue;
    }

    if event.reset_read().is_err() {
        return SampClientSdkHookAction::Continue;
    }

    let payload = match event.read_bits(bit_len) {
        Ok(payload) => payload,
        Err(_) => {
            let _ = event.reset_read();
            return SampClientSdkHookAction::Continue;
        }
    };

    let adapter = context.adapter();
    if adapter.is_null() {
        let _ = event.reset_read();
        return SampClientSdkHookAction::Continue;
    }

    let payload_ptr = if payload.is_empty() {
        ptr::null()
    } else {
        payload.as_ptr()
    };
    // Only consume packets that the negotiated extension adapter positively claims.
    let disposition = unsafe {
        custom_vehicle_adapter_handle_server_rpc(
            adapter,
            rpc_id,
            payload_ptr,
            payload.len(),
        )
    };

    if disposition > 0 {
        SampClientSdkHookAction::Block
    } else {
        let _ = event.reset_read();
        SampClientSdkHookAction::Continue
    }
}

fn handle_world_vehicle_add(
    context: &BridgeContext,
    event: &mut samp_client_sdk::events::Event<'_>,
) -> SampClientSdkHookAction {
    // Stock RPC 164 is exactly 40 bytes of vehicle data plus 23 bytes of tuning.
    // Leave non-matching payloads to the original client parser unchanged.
    if event.reset_read().is_err() || event.remaining_bits() != STREAM_IN_VEHICLE_BITS {
        let _ = event.reset_read();
        return SampClientSdkHookAction::Continue;
    }

    let payload = match event.read_bits(STREAM_IN_VEHICLE_BITS) {
        Ok(payload) if payload.len() == 63 => payload,
        _ => {
            // A failed read may have advanced the callback-local cursor.
            let _ = event.reset_read();
            return SampClientSdkHookAction::Continue;
        }
    };

    // Model ID is the signed 32-bit value at byte offset 2 in the packed RPC.
    let model_id = i32::from_le_bytes([
        payload[2],
        payload[3],
        payload[4],
        payload[5],
    ]);
    if !(MIN_CUSTOM_VEHICLE_MODEL_ID..=MAX_CUSTOM_VEHICLE_MODEL_ID).contains(&model_id) {
        let _ = event.reset_read();
        return SampClientSdkHookAction::Continue;
    }

    let adapter = context.adapter();
    let queue = context.pending_vehicle_queue();
    if adapter.is_null() || queue.is_null() {
        return SampClientSdkHookAction::Block;
    }

    let payload_ptr = payload.as_ptr();
    let mut decoded = CustomVehicleStreamInVehicleV1::default();
    let decode_status = unsafe {
        custom_vehicle_adapter_decode_stream_in_vehicle(
            adapter,
            payload_ptr,
            payload.len(),
            &mut decoded,
        )
    };
    if !(1..=2).contains(&decode_status)
        || decoded.custom_model_id != model_id
        || !(400..=611).contains(&decoded.base_model_id)
    {
        // Unknown custom model IDs and malformed records must never reach stock
        // CreateVehicle, which indexes the native keep-loaded table by model - 400.
        return SampClientSdkHookAction::Block;
    }

    let mut rewritten = [0u8; 63];
    let mut rewritten_size = 0usize;
    let mut rewritten_custom_id = 0i32;
    let mut rewritten_base_id = 0i32;
    let rewrite_status = unsafe {
        custom_vehicle_adapter_rewrite_stream_in_vehicle(
            adapter,
            payload_ptr,
            payload.len(),
            rewritten.as_mut_ptr(),
            rewritten.len(),
            &mut rewritten_size,
            &mut rewritten_custom_id,
            &mut rewritten_base_id,
        )
    };
    if !(1..=2).contains(&rewrite_status)
        || rewritten_size != rewritten.len()
        || rewritten_custom_id != model_id
        || rewritten_base_id != decoded.base_model_id
    {
        return SampClientSdkHookAction::Block;
    }

    // Replace only the model ID with the compatible stock base ID. The stock
    // client therefore constructs a valid base vehicle while the copied custom
    // record waits for the native game-thread registration/swap stage.
    if event.replace_bits(&rewritten, STREAM_IN_VEHICLE_BITS).is_err()
        || event.reset_read().is_err()
    {
        return SampClientSdkHookAction::Block;
    }

    let queue_result = unsafe { custom_vehicle_pending_vehicle_queue_enqueue(queue, &decoded) };
    if queue_result <= 0 {
        // Do not allow a base-only vehicle to spawn when its custom handoff could
        // not be queued. A later server stream-in can retry after queue pressure clears.
        return SampClientSdkHookAction::Block;
    }

    SampClientSdkHookAction::Continue
}

fn handle_world_vehicle_remove(
    context: &BridgeContext,
    event: &mut samp_client_sdk::events::Event<'_>,
) -> SampClientSdkHookAction {
    // RPC 165 carries a single 16-bit vehicle-pool ID. Cancellation is best-effort:
    // the stock remove handler always remains responsible for deleting the entity.
    if event.reset_read().is_ok() && event.remaining_bits() == 16 {
        if let Ok(payload) = event.read_bits(16) {
            if payload.len() == 2 {
                let vehicle_id = u16::from_le_bytes([payload[0], payload[1]]);
                let queue = context.pending_vehicle_queue();
                if !queue.is_null() {
                    unsafe { custom_vehicle_pending_vehicle_queue_cancel(queue, vehicle_id) };
                }
            }
        }
    }
    let _ = event.reset_read();
    SampClientSdkHookAction::Continue
}

fn handle_server_packet(
    context: &BridgeContext,
    event: &mut samp_client_sdk::events::Event<'_>,
) -> SampClientSdkHookAction {
    if is_connection_termination_packet(event.id()) {
        let _ = context.connection_events.send(ConnectionEvent::Disconnected);
    }

    // Connection-control packets remain owned by the original RakClient parser.
    SampClientSdkHookAction::Continue
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
    connection_events: Receiver<ConnectionEvent>,
    version: SampClientSdkClientVersion,
    ready: mpsc::SyncSender<bool>,
) {
    let samp = match Samp::connect(Duration::from_secs(30)) {
        Ok(samp) => samp,
        Err(_) => {
            let _ = ready.send(false);
            return;
        }
    };

    if samp.version().ok() != Some(version) || !supports_network_version(version) {
        let _ = ready.send(false);
        return;
    }
    let _ = ready.send(true);

    let net = samp.net();
    let native_game_state = has_native_game_state(version);
    let mut session_active = false;
    let mut init_game_seen = false;
    let mut disconnect_latched = false;

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
                        .and_then(|mut receipt| receipt.wait(Duration::from_secs(10)));
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

        while let Ok(event) = connection_events.try_recv() {
            match event {
                ConnectionEvent::GameInitialized => {
                    context.clear_pending_vehicles();
                    init_game_seen = true;
                    disconnect_latched = false;
                }
                ConnectionEvent::Disconnected => {
                    context.clear_pending_vehicles();
                    init_game_seen = false;
                    disconnect_latched = true;
                }
            }
        }

        if context.stopping.load(Ordering::Acquire) || !context.hooks_ready.load(Ordering::Acquire) {
            continue;
        }

        // Full native profiles additionally wait for state 14 (not pre-spawn state 15).
        // R2/R4-2 expose network hooks only, so their lifecycle is driven by InitGame/RakNet events.
        // The state fallback also supports loading this ASI after joining on full profiles.
        let connected = if disconnect_latched {
            false
        } else if native_game_state {
            samp.game_state()
                .map(|state| state == 14)
                .unwrap_or(init_game_seen)
        } else {
            init_game_seen
        };

        let adapter = context.adapter();
        if adapter.is_null() {
            continue;
        }

        if connected && !session_active {
            context.clear_pending_vehicles();
            if unsafe { custom_vehicle_adapter_begin_connection(adapter) } != 0 {
                session_active = true;
            }
        } else if !connected && session_active {
            context.clear_pending_vehicles();
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
    // Match the local artwork server's User-Agent allow-list.
    let agent = ureq::AgentBuilder::new()
        .user_agent("SAMP/0.3")
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
    // Resolve the cache root and entry first; never follow a cache symlink outside the root.
    let canonical_root = root.canonicalize().ok()?;
    let canonical_path = cache_path(root, job).canonicalize().ok()?;
    if !canonical_path.starts_with(&canonical_root) || !canonical_path.is_file() {
        return None;
    }

    let file = File::open(canonical_path).ok()?;
    if file.metadata().ok()?.len() != u64::from(job.expected_size) {
        return None;
    }

    let mut bytes = Vec::with_capacity(job.expected_size as usize);
    // Bound the read even if the cache file changes after the metadata check.
    file.take(u64::from(job.expected_size).saturating_add(1))
        .read_to_end(&mut bytes)
        .ok()?;
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
    let canonical_root = root.canonicalize()?;
    let destination = cache_path(&canonical_root, job);

    if read_verified_cache(&canonical_root, job).is_some() {
        return Ok(());
    }

    // Remove the link itself, not a path resolved through an existing symlink.
    match fs::symlink_metadata(&destination) {
        Ok(_) => fs::remove_file(&destination)?,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(error) => return Err(error),
    }

    let temporary = destination.with_extension(match job.asset_type {
        1 => "dff.part",
        _ => "txd.part",
    });
    match fs::symlink_metadata(&temporary) {
        Ok(_) => fs::remove_file(&temporary)?,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(error) => return Err(error),
    }

    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temporary)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    drop(file);

    let canonical_temporary = temporary.canonicalize()?;
    if !canonical_temporary.starts_with(&canonical_root) || !canonical_temporary.is_file() {
        let _ = fs::remove_file(&temporary);
        return Err(std::io::Error::new(
            std::io::ErrorKind::PermissionDenied,
            "temporary cache path escaped the configured cache root",
        ));
    }

    let mut persisted = Vec::with_capacity(job.expected_size as usize);
    File::open(canonical_temporary)?
        .take(u64::from(job.expected_size).saturating_add(1))
        .read_to_end(&mut persisted)?;
    if persisted.len() != job.expected_size as usize || crc32(&persisted) != job.checksum {
        fs::remove_file(&temporary)?;
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "persisted cache bytes failed verification",
        ));
    }

    fs::rename(&temporary, &destination)?;
    if read_verified_cache(&canonical_root, job).is_none() {
        let _ = fs::remove_file(&destination);
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "final cache entry failed path, size, or CRC32 verification",
        ));
    }
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

fn ensure_sdk_host_loaded() -> bool {
    let _guard = SDK_HOST_LOAD_LOCK
        .lock()
        .unwrap_or_else(|error| error.into_inner());

    // Reuse a host installed by another compatible plugin rather than loading a second copy.
    let existing = unsafe { GetModuleHandleA(c"samp_client_sdk.asi".as_ptr().cast()) };
    if !existing.is_null() {
        SDK_HOST_MODULE.store(existing.cast(), Ordering::Release);
        return true;
    }

    let Some(host_path) = write_embedded_sdk_host() else {
        return false;
    };
    let mut wide_path: Vec<u16> = host_path.as_os_str().encode_wide().collect();
    wide_path.push(0);

    // LoadLibrary invokes the SDK host's DllMain after this initialization worker
    // has left our own DllMain/loader-lock path.
    let module = unsafe { LoadLibraryW(wide_path.as_ptr()) };
    if module.is_null() {
        return false;
    }

    // Intentionally keep one loader reference for the rest of the process. Other ASI
    // plugins may use the shared SDK host after this adapter shuts down.
    SDK_HOST_MODULE.store(module.cast(), Ordering::Release);
    true
}

fn write_embedded_sdk_host() -> Option<PathBuf> {
    if EMBEDDED_SDK_HOST.is_empty() {
        return None;
    }

    let directory = cache_root().join("sdk-host");
    fs::create_dir_all(&directory).ok()?;
    let canonical_root = directory.canonicalize().ok()?;
    let destination = canonical_root.join("samp_client_sdk.asi");

    match fs::symlink_metadata(&destination) {
        Ok(metadata) => {
            if metadata.file_type().is_symlink() || !metadata.is_file() {
                return None;
            }
            if fs::read(&destination).ok()?.as_slice() == EMBEDDED_SDK_HOST {
                return Some(destination);
            }
            if let Err(error) = fs::remove_file(&destination) {
                // Another process may have replaced the stale entry after our read.
                if error.kind() != std::io::ErrorKind::NotFound {
                    return None;
                }
            }
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(_) => return None,
    }

    let temporary = canonical_root.join(format!(
        "samp_client_sdk.{}.tmp",
        std::process::id()
    ));
    match fs::symlink_metadata(&temporary) {
        Ok(metadata) => {
            if metadata.file_type().is_symlink() || !metadata.is_file() {
                return None;
            }
            fs::remove_file(&temporary).ok()?;
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(_) => return None,
    }

    let write_result = (|| -> std::io::Result<()> {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)?;
        file.write_all(EMBEDDED_SDK_HOST)?;
        file.sync_all()?;
        drop(file);

        let canonical_temporary = temporary.canonicalize()?;
        if !canonical_temporary.starts_with(&canonical_root) || !canonical_temporary.is_file() {
            return Err(std::io::Error::new(
                std::io::ErrorKind::PermissionDenied,
                "embedded SDK host temporary path escaped its cache directory",
            ));
        }
        fs::rename(&temporary, &destination)?;

        let persisted = fs::read(&destination)?;
        if persisted.as_slice() != EMBEDDED_SDK_HOST {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                "persisted SDK host differs from the embedded payload",
            ));
        }
        Ok(())
    })();

    if write_result.is_err() {
        let _ = fs::remove_file(&temporary);

        // A different GTA process may have won the atomic rename. Reuse its file
        // only after verifying the complete payload; never delete another process's
        // successful cache entry when our own rename loses the race.
        let existing_matches = fs::read(&destination)
            .map(|bytes| bytes.as_slice() == EMBEDDED_SDK_HOST)
            .unwrap_or(false);
        return existing_matches.then_some(destination);
    }

    Some(destination)
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

    let queue = context
        .pending_vehicles
        .swap(ptr::null_mut(), Ordering::AcqRel);
    if !queue.is_null() {
        unsafe {
            custom_vehicle_pending_vehicle_queue_clear(queue);
            let _ = custom_vehicle_pending_vehicle_queue_destroy(queue);
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
    native_tick: Option<NativeTickRegistration>,
) {
    context.stopping.store(true, Ordering::Release);
    if let Err(native_tick) = drain_native_game_tick(native_tick) {
        // Retain the context and callback registration if an in-flight game tick
        // could not be drained. Freeing user_data here would create a use-after-free.
        let mut state = STATE.lock().unwrap_or_else(|error| error.into_inner());
        state.context = Some(Arc::clone(context));
        state.subscriptions = subscriptions;
        state.workers = workers;
        state.native_tick = Some(native_tick);
        return;
    }

    if let Err(subscriptions) = drain_subscriptions(subscriptions) {
        // Retain every callback dependency until a later shutdown can drain active hooks.
        let mut state = STATE.lock().unwrap_or_else(|error| error.into_inner());
        state.context = Some(Arc::clone(context));
        state.subscriptions = subscriptions;
        state.workers = workers;
        state.native_tick = None;
        return;
    }

    stop_workers(context, workers);
}

#[unsafe(no_mangle)]
pub extern "system" fn CustomVehicleWindows_Shutdown() -> BOOL {
    let (context, subscriptions, workers, native_tick) = {
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
            state.native_tick.take(),
        )
    };

    let Some(context) = context else {
        return TRUE;
    };
    context.stopping.store(true, Ordering::Release);

    if let Err(native_tick) = drain_native_game_tick(native_tick) {
        // Keep the callback context alive when a game-thread callback is still in flight.
        let mut state = STATE.lock().unwrap_or_else(|error| error.into_inner());
        state.context = Some(context);
        state.subscriptions = subscriptions;
        state.workers = workers;
        state.native_tick = Some(native_tick);
        return 0;
    }

    if let Err(subscriptions) = drain_subscriptions(subscriptions) {
        // Keep the adapter, callback context, and workers alive if any hook may still be active.
        let mut state = STATE.lock().unwrap_or_else(|error| error.into_inner());
        state.context = Some(context);
        state.subscriptions = subscriptions;
        state.workers = workers;
        state.native_tick = None;
        return 0;
    }

    stop_workers(&context, workers);
    TRUE
}

#[unsafe(no_mangle)]
pub extern "system" fn CustomVehicleWindows_GameThreadCallbackRegistered() -> i32 {
    let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
    let Some(context) = state.context.as_ref() else {
        return 0;
    };
    if context.native_tick_registered.load(Ordering::Acquire) {
        1
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn CustomVehicleWindows_GameThreadTickCount() -> u32 {
    let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
    state
        .context
        .as_ref()
        .map(|context| context.game_thread_tick_count.load(Ordering::Acquire))
        .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "system" fn CustomVehicleWindows_PendingVehicleCountOnGameThread() -> usize {
    let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
    state
        .context
        .as_ref()
        .map(|context| {
            context
                .pending_vehicle_count_on_game_thread
                .load(Ordering::Acquire)
        })
        .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "system" fn CustomVehicleWindows_PendingVehicleBaseExistsOnGameThread() -> i32 {
    let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
    state
        .context
        .as_ref()
        .map(|context| i32::from(context.pending_vehicle_base_exists_on_game_thread.load(Ordering::Acquire)))
        .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "system" fn CustomVehicleWindows_PendingVehicleIdOnGameThread() -> u32 {
    let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
    state
        .context
        .as_ref()
        .map(|context| context.pending_vehicle_id_on_game_thread.load(Ordering::Acquire))
        .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "system" fn CustomVehicleWindows_PendingVehicleGtaHandleOnGameThread() -> i32 {
    let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
    state
        .context
        .as_ref()
        .map(|context| context.pending_vehicle_gta_handle_on_game_thread.load(Ordering::Acquire))
        .unwrap_or(-1)
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


/// Decode one server RPC 164 payload for the native vehicle backend.
///
/// Returns -1 for malformed/unknown custom models, 0 for a stock vehicle,
/// 1 for a registered custom model whose DFF/TXD are ready, or 2 while assets
/// are still pending. The output is not modified on error.
#[unsafe(no_mangle)]
pub unsafe extern "system" fn CustomVehicleWindows_DecodeStreamInVehicle(
    payload: *const u8,
    payload_size: usize,
    output: *mut CustomVehicleStreamInVehicleV1,
) -> i32 {
    if payload.is_null() || output.is_null() || payload_size == 0 || payload_size > MAX_RPC_BYTES {
        return -1;
    }

    std::panic::catch_unwind(|| {
        let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
        let Some(context) = state.context.as_ref() else {
            return -1;
        };
        let adapter = context.adapter();
        if adapter.is_null() {
            return -1;
        }

        unsafe {
            custom_vehicle_adapter_decode_stream_in_vehicle(
                adapter,
                payload,
                payload_size,
                output,
            )
        }
    })
    .unwrap_or(-1)
}

/// Prepare one exact server RPC 164 payload for the stock vehicle handler.
///
/// Returns -1 for invalid/unknown custom models or an inactive adapter, 0 for a
/// stock vehicle (no output is written), 1 for a registered model with verified
/// DFF/TXD assets, and 2 for a registered model whose assets are still pending.
/// For custom models, only the four-byte model ID field is rewritten to its
/// compatible base vehicle ID. The caller remains responsible for scheduling
/// native model registration and applying it on the verified game thread.
#[unsafe(no_mangle)]
pub unsafe extern "system" fn CustomVehicleWindows_RewriteStreamInVehicle(
    payload: *const u8,
    payload_size: usize,
    output: *mut u8,
    output_capacity: usize,
    output_size: *mut usize,
    custom_model_id: *mut i32,
    base_model_id: *mut i32,
) -> i32 {
    if payload.is_null()
        || output.is_null()
        || output_size.is_null()
        || custom_model_id.is_null()
        || base_model_id.is_null()
        || payload_size == 0
        || payload_size > MAX_RPC_BYTES
    {
        return -1;
    }

    std::panic::catch_unwind(|| {
        let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
        let Some(context) = state.context.as_ref() else {
            return -1;
        };
        let adapter = context.adapter();
        if adapter.is_null() {
            return -1;
        }

        unsafe {
            custom_vehicle_adapter_rewrite_stream_in_vehicle(
                adapter,
                payload,
                payload_size,
                output,
                output_capacity,
                output_size,
                custom_model_id,
                base_model_id,
            )
        }
    })
    .unwrap_or(-1)
}

/// Check whether both assets for a registered custom vehicle have been
/// verified locally and acknowledged by the server.
#[unsafe(no_mangle)]
pub extern "system" fn CustomVehicleWindows_IsModelReady(custom_model_id: i32) -> i32 {
    std::panic::catch_unwind(|| {
        let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
        let Some(context) = state.context.as_ref() else {
            return 0;
        };
        let adapter = context.adapter();
        if adapter.is_null() {
            return 0;
        }
        unsafe { custom_vehicle_adapter_is_model_ready(adapter, custom_model_id) }
    })
    .unwrap_or(0)
}

/// Number of copied custom vehicle stream-in records waiting for game-thread work.
#[unsafe(no_mangle)]
pub extern "system" fn CustomVehicleWindows_PendingVehicleCount() -> usize {
    let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
    let Some(context) = state.context.as_ref() else {
        return 0;
    };
    let queue = context.pending_vehicle_queue();
    if queue.is_null() {
        return 0;
    }
    unsafe { custom_vehicle_pending_vehicle_queue_size(queue) }
}

/// Pop one copied stream-in record. Call only from the verified GTA game thread.
#[unsafe(no_mangle)]
pub unsafe extern "system" fn CustomVehicleWindows_PopPendingVehicle(
    output: *mut CustomVehicleStreamInVehicleV1,
) -> i32 {
    if output.is_null() {
        return -1;
    }

    std::panic::catch_unwind(|| {
        let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
        let Some(context) = state.context.as_ref() else {
            return -1;
        };
        let queue = context.pending_vehicle_queue();
        if queue.is_null() {
            return -1;
        }
        unsafe { custom_vehicle_pending_vehicle_queue_pop(queue, output) }
    })
    .unwrap_or(-1)
}

/// Cancel a not-yet-popped stream-in record by SA-MP vehicle-pool ID.
#[unsafe(no_mangle)]
pub extern "system" fn CustomVehicleWindows_CancelPendingVehicle(vehicle_id: u16) -> i32 {
    let state = STATE.lock().unwrap_or_else(|error| error.into_inner());
    let Some(context) = state.context.as_ref() else {
        return 0;
    };
    let queue = context.pending_vehicle_queue();
    if queue.is_null() {
        return 0;
    }
    unsafe { custom_vehicle_pending_vehicle_queue_cancel(queue, vehicle_id) }
}

#[cfg(test)]
mod tests {
    use super::{
        cache_path, crc32, custom_vehicle_pending_vehicle_queue_cancel,
        custom_vehicle_pending_vehicle_queue_clear, custom_vehicle_pending_vehicle_queue_create,
        custom_vehicle_pending_vehicle_queue_destroy, custom_vehicle_pending_vehicle_queue_enqueue,
        custom_vehicle_pending_vehicle_queue_pop, custom_vehicle_pending_vehicle_queue_size,
        is_connection_termination_packet, is_custom_vehicle_rpc, read_verified_cache,
        store_verified_cache, CustomVehicleStreamInVehicleV1, DownloadJob,
        SampClientSdkClientVersion,
    };
    use std::fs;

    #[test]
    fn only_server_to_client_vehicle_rpcs_are_classified() {
        for rpc_id in [220, 222, 225, 227] {
            assert!(is_custom_vehicle_rpc(rpc_id), "server extension RPC {rpc_id} was not classified");
        }

        for rpc_id in [179, 181, 182, 183, 184, 185, 186, 187, 188, 189, 219, 221, 223, 224, 226, 228, 255] {
            assert!(
                !is_custom_vehicle_rpc(rpc_id),
                "native or unrelated RPC {rpc_id} was classified as a vehicle extension"
            );
        }
    }

    #[test]
    fn stream_in_vehicle_export_layout_matches_the_shared_c_abi() {
        assert_eq!(std::mem::size_of::<CustomVehicleStreamInVehicleV1>(), 76);
        assert_eq!(std::mem::offset_of!(CustomVehicleStreamInVehicleV1, vehicle_id), 0);
        assert_eq!(std::mem::offset_of!(CustomVehicleStreamInVehicleV1, custom_model_id), 4);
        assert_eq!(std::mem::offset_of!(CustomVehicleStreamInVehicleV1, position), 12);
        assert_eq!(std::mem::offset_of!(CustomVehicleStreamInVehicleV1, health), 32);
        assert_eq!(std::mem::offset_of!(CustomVehicleStreamInVehicleV1, mods), 51);
        assert_eq!(std::mem::offset_of!(CustomVehicleStreamInVehicleV1, body_colour1), 68);
    }

    #[test]
    fn pending_vehicle_queue_c_abi_coalesces_cancels_and_preserves_latest_record() {
        let queue = unsafe { custom_vehicle_pending_vehicle_queue_create() };
        assert!(!queue.is_null());

        let mut first = CustomVehicleStreamInVehicleV1 {
            vehicle_id: 42,
            custom_model_id: 30_123,
            base_model_id: 400,
            position: [12.0, 34.0, 56.0],
            angle: 90.0,
            health: 950.0,
            ..CustomVehicleStreamInVehicleV1::default()
        };
        assert_eq!(
            unsafe { custom_vehicle_pending_vehicle_queue_enqueue(queue, &first) },
            1
        );

        // A repeated stream-in for the same pending vehicle replaces its snapshot.
        first.angle = 180.0;
        first.siren = 1;
        first.paintjob = 2;
        assert_eq!(
            unsafe { custom_vehicle_pending_vehicle_queue_enqueue(queue, &first) },
            2
        );
        assert_eq!(unsafe { custom_vehicle_pending_vehicle_queue_size(queue) }, 1);

        let mut copied = CustomVehicleStreamInVehicleV1::default();
        assert_eq!(
            unsafe { custom_vehicle_pending_vehicle_queue_pop(queue, &mut copied) },
            1
        );
        assert_eq!(copied.vehicle_id, 42);
        assert_eq!(copied.custom_model_id, 30_123);
        assert_eq!(copied.base_model_id, 400);
        assert_eq!(copied.angle, 180.0);
        assert_eq!(copied.siren, 1);
        assert_eq!(copied.paintjob, 2);
        assert_eq!(copied.position, [12.0, 34.0, 56.0]);
        assert_eq!(unsafe { custom_vehicle_pending_vehicle_queue_pop(queue, &mut copied) }, 0);
        assert_eq!(unsafe { custom_vehicle_pending_vehicle_queue_size(queue) }, 0);

        // Remove RPC cancellation prevents a not-yet-popped vehicle from leaking into the game thread.
        assert_eq!(
            unsafe { custom_vehicle_pending_vehicle_queue_enqueue(queue, &first) },
            1
        );
        assert_eq!(unsafe { custom_vehicle_pending_vehicle_queue_cancel(queue, 42) }, 1);
        assert_eq!(unsafe { custom_vehicle_pending_vehicle_queue_size(queue) }, 0);
        assert_eq!(unsafe { custom_vehicle_pending_vehicle_queue_cancel(queue, 42) }, 0);

        // A destroyed queue can never retain a pending record.
        unsafe { custom_vehicle_pending_vehicle_queue_clear(queue) };
        assert_eq!(unsafe { custom_vehicle_pending_vehicle_queue_destroy(queue) }, 1);
    }

    #[test]
    fn network_hook_support_covers_all_recognized_sa_mp_profiles() {
        for version in [
            SampClientSdkClientVersion::R1,
            SampClientSdkClientVersion::R2,
            SampClientSdkClientVersion::R3_1,
            SampClientSdkClientVersion::R4_2,
            SampClientSdkClientVersion::R5_1,
            SampClientSdkClientVersion::Dl,
        ] {
            assert!(super::supports_network_version(version));
        }
    }

    #[test]
    fn only_profiles_with_verified_native_state_layouts_use_state_14_gate() {
        for version in [
            SampClientSdkClientVersion::R1,
            SampClientSdkClientVersion::R3_1,
            SampClientSdkClientVersion::R5_1,
            SampClientSdkClientVersion::Dl,
        ] {
            assert!(super::has_native_game_state(version));
        }
        for version in [
            SampClientSdkClientVersion::R2,
            SampClientSdkClientVersion::R4_2,
        ] {
            assert!(!super::has_native_game_state(version));
        }
    }

    #[test]
    fn sa_mp_connection_failure_and_disconnect_packets_are_recognized() {
        for packet_id in [29, 31, 32, 33, 36, 37] {
            assert!(is_connection_termination_packet(packet_id));
        }
        for packet_id in [16, 17, 34, 42, 43, 44, 207, 220] {
            assert!(!is_connection_termination_packet(packet_id));
        }
    }

    #[test]
    fn cache_crc32_matches_standard_vector() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn cache_round_trip_and_tamper_detection() {
        use std::time::{SystemTime, UNIX_EPOCH};

        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock must be after Unix epoch")
            .as_nanos();
        let root = std::env::temp_dir().join(format!("openmp-custom-vehicle-{unique}"));
        let bytes = b"custom vehicle cache test";
        let job = DownloadJob {
            asset_type: 1,
            checksum: crc32(bytes),
            expected_size: bytes.len() as u32,
            url: "https://example.test/model.dff".to_owned(),
        };

        store_verified_cache(&root, &job, bytes).expect("verified cache write should succeed");
        assert_eq!(read_verified_cache(&root, &job).as_deref(), Some(bytes.as_slice()));

        let path = cache_path(&root, &job);
        fs::write(&path, b"modified cache entry").expect("test should modify the cache file");
        assert!(read_verified_cache(&root, &job).is_none());

        fs::remove_dir_all(&root).expect("test cache directory should be removable");
    }
}

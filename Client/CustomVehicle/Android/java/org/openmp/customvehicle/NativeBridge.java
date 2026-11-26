package org.openmp.customvehicle;

import java.io.File;
import java.util.Locale;
import java.util.Objects;

/**
 * JNI hand-off point for an Android multiplayer client integration.
 *
 * The client must call {@link #handleServerRpc(int, byte[])} from its incoming
 * RPC hook and apply the returned disposition: 1 consumes the RPC, 0 forwards
 * it to the stock client, and -1 indicates invalid arguments.
 */
public final class NativeBridge implements AutoCloseable {
    public interface Callbacks {
        /**
         * Enqueue an outgoing RPC on the multiplayer network thread.
         * Do not synchronously re-enter NativeBridge from this callback.
         */
        boolean sendRpc(int rpcId, byte[] payload);

        /**
         * Copy the URL/metadata and schedule HTTP/file I/O on a worker thread.
         * Do not download synchronously in the network callback.
         */
        boolean beginDownload(int assetType, long checksum, long expectedSize, String url);
    }

    private long handle;
    private final String cacheDirectory;

    static {
        System.loadLibrary("custom_vehicle_android");
    }

    public NativeBridge(Callbacks callbacks, File appPrivateCacheDirectory) {
        Objects.requireNonNull(callbacks, "callbacks");
        Objects.requireNonNull(appPrivateCacheDirectory, "appPrivateCacheDirectory");

        this.cacheDirectory = appPrivateCacheDirectory.getAbsolutePath();
        if (!appPrivateCacheDirectory.isDirectory() && !appPrivateCacheDirectory.mkdirs()) {
            throw new IllegalStateException("Unable to create custom vehicle cache directory");
        }

        this.handle = nativeCreate(callbacks, cacheDirectory);
        if (handle == 0) {
            throw new IllegalStateException("Unable to initialize custom vehicle JNI bridge");
        }
    }

    public synchronized boolean beginConnection() {
        return handle != 0 && nativeBeginConnection(handle);
    }

    public synchronized void endConnection() {
        if (handle != 0) {
            nativeEndConnection(handle);
        }
    }

    /**
     * Returns 1 to consume, 0 to forward to the original client, or -1 on error.
     */
    public synchronized int handleServerRpc(int rpcId, byte[] payload) {
        Objects.requireNonNull(payload, "payload");
        return handle == 0 ? -1 : nativeHandleServerRpc(handle, rpcId, payload);
    }

    /**
     * Persist and verify the completed asset before the bridge sends vehicle finish RPC 226.
     * The expected length and checksum are the exact values supplied to beginDownload.
     */
    public synchronized boolean completeAsset(
            int assetType,
            long checksum,
            long expectedSize,
            byte[] data) {
        Objects.requireNonNull(data, "data");
        return handle != 0
                && nativeCompleteAsset(handle, assetType, checksum, expectedSize, data);
    }

    /** Retry after a platform downloader has applied its own timeout/backoff policy. */
    public synchronized boolean retryActiveAsset() {
        return handle != 0 && nativeRetryActiveAsset(handle);
    }

    /**
     * Returns the model fields in this order:
     * poolId, modelCount, type, virtualWorld, baseId, newId,
     * dffChecksum, txdChecksum, dffSize, txdSize, timeOn, timeOff.
     */
    public synchronized long[] copyModel(int index) {
        return handle == 0 ? null : nativeCopyModel(handle, index);
    }

    /**
     * Checks the on-disk asset size and CRC32 again before exposing it to a model loader.
     * Call this after completeAsset() returns true.
     */
    public synchronized boolean hasAsset(int assetType, long checksum, long expectedSize) {
        if ((assetType != 1 && assetType != 2)
                || checksum < 0 || checksum > 0xFFFF_FFFFL
                || expectedSize <= 0 || expectedSize > 0xFFFF_FFFFL) {
            return false;
        }
        return handle != 0 && nativeHasAsset(handle, assetType, checksum, expectedSize);
    }

    /** Returns the deterministic cache path; check hasAsset() before passing it to a loader. */
    public String assetPath(int assetType, long checksum) {
        if ((assetType != 1 && assetType != 2) || checksum < 0 || checksum > 0xFFFF_FFFFL) {
            throw new IllegalArgumentException("Invalid asset type or CRC32");
        }
        String prefix = assetType == 1 ? "dff-" : "txd-";
        String extension = assetType == 1 ? ".dff" : ".txd";
        String fileName = prefix + String.format(Locale.ROOT, "%08x", checksum) + extension;
        return new File(cacheDirectory, fileName).getAbsolutePath();
    }

    /**
     * Stop/join all download workers before closing the bridge. A worker must
     * never call completeAsset after this method returns.
     */
    @Override
    public synchronized void close() {
        if (handle != 0) {
            nativeEndConnection(handle);
            nativeDestroy(handle);
            handle = 0;
        }
    }

    private static native long nativeCreate(Callbacks callbacks, String cacheDirectory);
    private static native void nativeDestroy(long handle);
    private static native boolean nativeBeginConnection(long handle);
    private static native void nativeEndConnection(long handle);
    private static native int nativeHandleServerRpc(long handle, int rpcId, byte[] payload);
    private static native boolean nativeCompleteAsset(
            long handle, int assetType, long checksum, long expectedSize, byte[] data);
    private static native boolean nativeHasAsset(
            long handle, int assetType, long checksum, long expectedSize);
    private static native boolean nativeRetryActiveAsset(long handle);
    private static native long[] nativeCopyModel(long handle, int index);
}

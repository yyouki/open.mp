#include <jni.h>

#include <custom_vehicle/asset_cache.hpp>
#include <custom_vehicle/bridge.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
struct AndroidBridgeContext
{
	JavaVM* vm = nullptr;
	jobject callbacks = nullptr;
	jmethodID sendRpcMethod = nullptr;
	jmethodID beginDownloadMethod = nullptr;
	std::filesystem::path cacheRoot;
	custom_vehicle::protocol::AssetCache cache;
	custom_vehicle_adapter* adapter = nullptr;

	AndroidBridgeContext(
		JavaVM* javaVm,
		jobject globalCallbacks,
		jmethodID sendMethod,
		jmethodID downloadMethod,
		std::filesystem::path root)
		: vm(javaVm)
		, callbacks(globalCallbacks)
		, sendRpcMethod(sendMethod)
		, beginDownloadMethod(downloadMethod)
		, cacheRoot(std::move(root))
		, cache(cacheRoot)
	{
	}
};

class ScopedJNIEnv
{
public:
	explicit ScopedJNIEnv(JavaVM* vm) noexcept
		: vm_(vm)
	{
		if (vm_ == nullptr)
		{
			return;
		}

		const jint status = vm_->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6);
		if (status == JNI_OK)
		{
			return;
		}
		if (status == JNI_EDETACHED
			&& vm_->AttachCurrentThread(&env_, nullptr) == JNI_OK)
		{
			attached_ = true;
		}
	}

	~ScopedJNIEnv()
	{
		if (attached_ && vm_ != nullptr)
		{
			vm_->DetachCurrentThread();
		}
	}

	JNIEnv* get() const noexcept
	{
		return env_;
	}

private:
	JavaVM* vm_ = nullptr;
	JNIEnv* env_ = nullptr;
	bool attached_ = false;
};

bool clearJavaException(JNIEnv* env) noexcept
{
	if (env == nullptr || !env->ExceptionCheck())
	{
		return false;
	}
	env->ExceptionClear();
	return true;
}

bool appendUtf8(std::string& output, std::uint32_t codePoint)
{
	if (codePoint <= 0x7Fu)
	{
		output.push_back(static_cast<char>(codePoint));
	}
	else if (codePoint <= 0x7FFu)
	{
		output.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
		output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
	}
	else if (codePoint <= 0xFFFFu)
	{
		output.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
		output.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
		output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
	}
	else if (codePoint <= 0x10FFFFu)
	{
		output.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
		output.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
		output.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
		output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
	}
	else
	{
		return false;
	}
	return true;
}

bool getUtf8String(JNIEnv* env, jstring value, std::string& output)
{
	if (env == nullptr || value == nullptr)
	{
		return false;
	}

	const jsize length = env->GetStringLength(value);
	const jchar* characters = env->GetStringChars(value, nullptr);
	if (characters == nullptr)
	{
		clearJavaException(env);
		return false;
	}

	bool valid = true;
	output.clear();
	output.reserve(static_cast<std::size_t>(length) * 3u);
	for (jsize index = 0; index < length && valid; ++index)
	{
		std::uint32_t codePoint = characters[index];
		if (codePoint >= 0xD800u && codePoint <= 0xDBFFu)
		{
			if (index + 1 >= length)
			{
				valid = false;
				break;
			}
			const std::uint32_t low = characters[++index];
			if (low < 0xDC00u || low > 0xDFFFu)
			{
				valid = false;
				break;
			}
			codePoint = 0x10000u + ((codePoint - 0xD800u) << 10) + (low - 0xDC00u);
		}
		else if (codePoint >= 0xDC00u && codePoint <= 0xDFFFu)
		{
			valid = false;
			break;
		}
		valid = appendUtf8(output, codePoint);
	}

	env->ReleaseStringChars(value, characters);
	if (!valid || env->ExceptionCheck())
	{
		clearJavaException(env);
		output.clear();
		return false;
	}
	return true;
}

std::int32_t sendRpcCallback(
	void* userData,
	std::uint8_t rpcId,
	const std::uint8_t* payload,
	std::size_t payloadSize)
{
	auto* context = static_cast<AndroidBridgeContext*>(userData);
	if (context == nullptr || (payload == nullptr && payloadSize != 0)
		|| payloadSize > static_cast<std::size_t>(std::numeric_limits<jsize>::max()))
	{
		return 0;
	}

	ScopedJNIEnv scopedEnv(context->vm);
	JNIEnv* env = scopedEnv.get();
	if (env == nullptr)
	{
		return 0;
	}

	jbyteArray bytes = env->NewByteArray(static_cast<jsize>(payloadSize));
	if (bytes == nullptr)
	{
		clearJavaException(env);
		return 0;
	}
	if (payloadSize != 0)
	{
		env->SetByteArrayRegion(
			bytes,
			0,
			static_cast<jsize>(payloadSize),
			reinterpret_cast<const jbyte*>(payload));
	}
	if (clearJavaException(env))
	{
		env->DeleteLocalRef(bytes);
		return 0;
	}

	const jboolean accepted = env->CallBooleanMethod(
		context->callbacks,
		context->sendRpcMethod,
		static_cast<jint>(rpcId),
		bytes);
	const bool exception = clearJavaException(env);
	env->DeleteLocalRef(bytes);
	return !exception && accepted == JNI_TRUE ? 1 : 0;
}

std::int32_t beginDownloadCallback(
	void* userData,
	std::uint8_t assetType,
	std::uint32_t checksum,
	std::uint32_t expectedSize,
	const char* url,
	std::size_t urlSize)
{
	auto* context = static_cast<AndroidBridgeContext*>(userData);
	if (context == nullptr || (url == nullptr && urlSize != 0)
		|| urlSize > custom_vehicle::protocol::MaxDownloadUrlBytes)
	{
		return 0;
	}

	// The protocol validator restricts URLs to ASCII, making NewStringUTF safe here.
	const std::string urlString(url == nullptr ? "" : std::string(url, urlSize));
	ScopedJNIEnv scopedEnv(context->vm);
	JNIEnv* env = scopedEnv.get();
	if (env == nullptr)
	{
		return 0;
	}

	jstring javaUrl = env->NewStringUTF(urlString.c_str());
	if (javaUrl == nullptr)
	{
		clearJavaException(env);
		return 0;
	}

	const jboolean accepted = env->CallBooleanMethod(
		context->callbacks,
		context->beginDownloadMethod,
		static_cast<jint>(assetType),
		static_cast<jlong>(checksum),
		static_cast<jlong>(expectedSize),
		javaUrl);
	const bool exception = clearJavaException(env);
	env->DeleteLocalRef(javaUrl);
	return !exception && accepted == JNI_TRUE ? 1 : 0;
}

AndroidBridgeContext* fromHandle(jlong handle) noexcept
{
	return reinterpret_cast<AndroidBridgeContext*>(static_cast<std::intptr_t>(handle));
}

bool validUnsigned32(jlong value) noexcept
{
	return value >= 0 && static_cast<std::uint64_t>(value) <= 0xFFFFFFFFull;
}
} // namespace

extern "C"
{
JNIEXPORT jlong JNICALL
Java_org_openmp_customvehicle_NativeBridge_nativeCreate(
	JNIEnv* env,
	jclass,
	jobject callbacks,
	jstring cacheDirectory)
{
	if (env == nullptr || callbacks == nullptr || cacheDirectory == nullptr)
	{
		return 0;
	}

	JavaVM* vm = nullptr;
	if (env->GetJavaVM(&vm) != JNI_OK || vm == nullptr)
	{
		return 0;
	}

	jclass callbackClass = env->GetObjectClass(callbacks);
	if (callbackClass == nullptr)
	{
		clearJavaException(env);
		return 0;
	}

	const jmethodID sendRpcMethod = env->GetMethodID(callbackClass, "sendRpc", "(I[B)Z");
	const jmethodID beginDownloadMethod =
		env->GetMethodID(callbackClass, "beginDownload", "(IJJLjava/lang/String;)Z");
	const bool methodError = clearJavaException(env);
	env->DeleteLocalRef(callbackClass);
	if (methodError || sendRpcMethod == nullptr || beginDownloadMethod == nullptr)
	{
		return 0;
	}

	std::string cacheRoot;
	if (!getUtf8String(env, cacheDirectory, cacheRoot) || cacheRoot.empty())
	{
		return 0;
	}

	jobject globalCallbacks = env->NewGlobalRef(callbacks);
	if (globalCallbacks == nullptr)
	{
		clearJavaException(env);
		return 0;
	}

	AndroidBridgeContext* context = nullptr;
	try
	{
		context = new AndroidBridgeContext(
			vm,
			globalCallbacks,
			sendRpcMethod,
			beginDownloadMethod,
			std::filesystem::u8path(cacheRoot));
	}
	catch (...)
	{
		env->DeleteGlobalRef(globalCallbacks);
		return 0;
	}

	custom_vehicle_callbacks_v1 bridgeCallbacks {};
	bridgeCallbacks.struct_size = static_cast<std::uint32_t>(sizeof(bridgeCallbacks));
	bridgeCallbacks.abi_version = CUSTOM_VEHICLE_BRIDGE_ABI_VERSION;
	bridgeCallbacks.user_data = context;
	bridgeCallbacks.send_rpc = sendRpcCallback;
	bridgeCallbacks.begin_download = beginDownloadCallback;
	context->adapter = custom_vehicle_adapter_create(&bridgeCallbacks);
	if (context->adapter == nullptr)
	{
		env->DeleteGlobalRef(globalCallbacks);
		delete context;
		return 0;
	}

	return static_cast<jlong>(reinterpret_cast<std::intptr_t>(context));
}

JNIEXPORT void JNICALL
Java_org_openmp_customvehicle_NativeBridge_nativeDestroy(JNIEnv* env, jclass, jlong handle)
{
	AndroidBridgeContext* context = fromHandle(handle);
	if (context == nullptr)
	{
		return;
	}

	custom_vehicle_adapter_end_connection(context->adapter);
	custom_vehicle_adapter_destroy(context->adapter);
	if (env != nullptr && context->callbacks != nullptr)
	{
		env->DeleteGlobalRef(context->callbacks);
	}
	context->adapter = nullptr;
	delete context;
}

JNIEXPORT jboolean JNICALL
Java_org_openmp_customvehicle_NativeBridge_nativeBeginConnection(JNIEnv*, jclass, jlong handle)
{
	AndroidBridgeContext* context = fromHandle(handle);
	return context != nullptr && custom_vehicle_adapter_begin_connection(context->adapter) != 0
		? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_openmp_customvehicle_NativeBridge_nativeEndConnection(JNIEnv*, jclass, jlong handle)
{
	AndroidBridgeContext* context = fromHandle(handle);
	if (context != nullptr)
	{
		custom_vehicle_adapter_end_connection(context->adapter);
	}
}

JNIEXPORT jint JNICALL
Java_org_openmp_customvehicle_NativeBridge_nativeHandleServerRpc(
	JNIEnv* env,
	jclass,
	jlong handle,
	jint rpcId,
	jbyteArray payload)
{
	AndroidBridgeContext* context = fromHandle(handle);
	if (context == nullptr || env == nullptr || payload == nullptr || rpcId < 0 || rpcId > 255)
	{
		return -1;
	}

	const jsize length = env->GetArrayLength(payload);
	if (length < 0)
	{
		return -1;
	}
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
	if (length != 0)
	{
		env->GetByteArrayRegion(payload, 0, length, reinterpret_cast<jbyte*>(bytes.data()));
		if (clearJavaException(env))
		{
			return -1;
		}
	}

	return custom_vehicle_adapter_handle_server_rpc(
		context->adapter,
		static_cast<std::uint8_t>(rpcId),
		bytes.empty() ? nullptr : bytes.data(),
		bytes.size());
}

JNIEXPORT jboolean JNICALL
Java_org_openmp_customvehicle_NativeBridge_nativeCompleteAsset(
	JNIEnv* env,
	jclass,
	jlong handle,
	jint assetType,
	jlong checksum,
	jlong expectedSize,
	jbyteArray data)
{
	AndroidBridgeContext* context = fromHandle(handle);
	if (context == nullptr || env == nullptr || data == nullptr
		|| !validUnsigned32(checksum) || !validUnsigned32(expectedSize))
	{
		return JNI_FALSE;
	}

	const jsize length = env->GetArrayLength(data);
	if (length < 0 || static_cast<std::uint64_t>(length) != static_cast<std::uint64_t>(expectedSize))
	{
		return JNI_FALSE;
	}

	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
	if (length != 0)
	{
		env->GetByteArrayRegion(data, 0, length, reinterpret_cast<jbyte*>(bytes.data()));
		if (clearJavaException(env))
		{
			return JNI_FALSE;
		}
	}

	custom_vehicle::protocol::AssetType type;
	switch (assetType)
	{
		case static_cast<jint>(custom_vehicle::protocol::AssetType::Dff):
			type = custom_vehicle::protocol::AssetType::Dff;
			break;
		case static_cast<jint>(custom_vehicle::protocol::AssetType::Txd):
			type = custom_vehicle::protocol::AssetType::Txd;
			break;
		default:
			return JNI_FALSE;
	}

	const std::uint32_t crc = static_cast<std::uint32_t>(checksum);
	const std::uint32_t expected = static_cast<std::uint32_t>(expectedSize);
	if (!context->cache.store(type, crc, expected, bytes.data(), bytes.size()))
	{
		return JNI_FALSE;
	}

	return custom_vehicle_adapter_complete_asset(
		context->adapter,
		static_cast<std::uint8_t>(type),
		crc,
		bytes.data(),
		bytes.size()) != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_openmp_customvehicle_NativeBridge_nativeRetryActiveAsset(JNIEnv*, jclass, jlong handle)
{
	AndroidBridgeContext* context = fromHandle(handle);
	return context != nullptr && custom_vehicle_adapter_retry_active_asset(context->adapter) != 0
		? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlongArray JNICALL
Java_org_openmp_customvehicle_NativeBridge_nativeCopyModel(
	JNIEnv* env,
	jclass,
	jlong handle,
	jint index)
{
	AndroidBridgeContext* context = fromHandle(handle);
	if (context == nullptr || env == nullptr || index < 0)
	{
		return nullptr;
	}

	custom_vehicle_model_request_v1 model {};
	if (custom_vehicle_adapter_copy_model(
		context->adapter, static_cast<std::size_t>(index), &model) == 0)
	{
		return nullptr;
	}

	const jlong fields[] = {
		static_cast<jlong>(model.pool_id),
		static_cast<jlong>(model.model_count),
		static_cast<jlong>(model.type),
		static_cast<jlong>(model.virtual_world),
		static_cast<jlong>(model.base_id),
		static_cast<jlong>(model.new_id),
		static_cast<jlong>(model.dff_checksum),
		static_cast<jlong>(model.txd_checksum),
		static_cast<jlong>(model.dff_size),
		static_cast<jlong>(model.txd_size),
		static_cast<jlong>(model.time_on),
		static_cast<jlong>(model.time_off)
	};

	jlongArray result = env->NewLongArray(static_cast<jsize>(sizeof(fields) / sizeof(fields[0])));
	if (result == nullptr)
	{
		clearJavaException(env);
		return nullptr;
	}
	env->SetLongArrayRegion(
		result,
		0,
		static_cast<jsize>(sizeof(fields) / sizeof(fields[0])),
		fields);
	if (clearJavaException(env))
	{
		env->DeleteLocalRef(result);
		return nullptr;
	}
	return result;
}
} // extern "C"

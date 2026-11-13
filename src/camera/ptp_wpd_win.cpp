// The system's WPD devices for ptp_wpd.hpp: IPortableDeviceManager to list them, IPortableDevice::SendCommand with the
// MTP-extension commands to talk PTP. The device object is the free-threaded one (CLSID_PortableDeviceFTM): the camera
// is opened on the GUI thread and used from the capture threads.
#include "camera/ptp_wpd.hpp"

#include <windows.h>
#include <objbase.h>
#include <portabledeviceapi.h>
#include <portabledevicetypes.h>

#include <format>
#include <utility>

namespace slopcap::ptp::wpd {

namespace {

// The GUIDs and keys used, as the Windows SDK defines them (mingw-w64's headers lack the MTP-extension ones; defining
// them all here keeps the GUID libraries out of the link).
constexpr GUID kClsidManager = {0x0af10cec, 0x2ecd, 0x4b92, {0x95, 0x81, 0x34, 0xf6, 0xae, 0x06, 0x37, 0xf3}};
constexpr GUID kClsidDeviceFtm = {0xf7c0039a, 0x4762, 0x488a, {0xb4, 0xb3, 0x76, 0x0e, 0xf9, 0xa1, 0xba, 0x9b}};
constexpr GUID kClsidValues = {0x0c15d503, 0xd017, 0x47ce, {0x90, 0x16, 0x7b, 0x3f, 0x97, 0x87, 0x21, 0xcc}};
constexpr GUID kClsidPropVariants = {0x08a99e2f, 0x6d6d, 0x4b80, {0xaf, 0x5a, 0xba, 0xf2, 0xbc, 0xbe, 0x4c, 0xb9}};
constexpr GUID kMtpExt = {0x4d545058, 0x1a2e, 0x4106, {0xa3, 0x57, 0x77, 0x1e, 0x08, 0x19, 0xfc, 0x56}};   // WPD_CATEGORY_MTP_EXT_VENDOR_OPERATIONS
constexpr GUID kCommon = {0xf0422a9c, 0x5dc8, 0x4440, {0xb5, 0xbd, 0x5d, 0xf2, 0x88, 0x35, 0x65, 0x8a}};
constexpr GUID kClient = {0x204d9f0c, 0x2292, 0x4080, {0x9f, 0x42, 0x40, 0x66, 0x4e, 0x70, 0xf8, 0x59}};
constexpr PROPERTYKEY kCommandCategory{kCommon, 1001}, kCommandId{kCommon, 1002}, kHresult{kCommon, 1003};
constexpr PROPERTYKEY kOpCode{kMtpExt, 1001}, kOpParams{kMtpExt, 1002}, kRespCode{kMtpExt, 1003}, kRespParams{kMtpExt, 1004},
    kVendorOps{kMtpExt, 1005}, kContext{kMtpExt, 1006}, kTotal{kMtpExt, 1007}, kToRead{kMtpExt, 1008}, kRead{kMtpExt, 1009},
    kToWrite{kMtpExt, 1010}, kWritten{kMtpExt, 1011}, kData{kMtpExt, 1012}, kOptimal{kMtpExt, 1013};
constexpr PROPERTYKEY kClientName{kClient, 2}, kClientMajor{kClient, 3}, kClientMinor{kClient, 4}, kClientRevision{kClient, 5},
    kClientQos{kClient, 8};

template <class T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    Com& operator=(Com&&) = delete;
    T** put() {
        reset();
        return &p_;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    T* p_ = nullptr;
};

/// COM on this thread, once: the caller's apartment if it has one (the GUI thread's), else the multithreaded one.
void ensureCom() {
    thread_local bool done = false;
    if (done) return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // (RPC_E_CHANGED_MODE: already single-threaded, also fine)
    done = true;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

Com<IPortableDeviceValues> newValues() {
    Com<IPortableDeviceValues> v;
    const HRESULT hr = CoCreateInstance(kClsidValues, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IPortableDeviceValues), v.putVoid());
    if (FAILED(hr)) throw Error(std::format("WPD: create a value set: {}", hresultText(hr)));
    return v;
}

class SystemDevice final : public Device {
public:
    explicit SystemDevice(const std::string& id) {
        ensureCom();
        HRESULT hr = CoCreateInstance(kClsidDeviceFtm, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IPortableDevice), dev_.putVoid());
        if (FAILED(hr)) throw CameraError(std::format("WPD: create a device object: {}", hresultText(hr)));
        auto client = newValues();
        client->SetStringValue(kClientName, L"SlopCap");
        client->SetUnsignedIntegerValue(kClientMajor, 0);
        client->SetUnsignedIntegerValue(kClientMinor, 1);
        client->SetUnsignedIntegerValue(kClientRevision, 0);
        client->SetUnsignedIntegerValue(kClientQos, SECURITY_IMPERSONATION);
        hr = dev_->Open(wide(id).c_str(), client.get());
        if (hr == E_ACCESSDENIED)
            throw CameraError(std::format("open {}: access denied (in use by another program, such as Imaging Edge or a photo importer?)", usbIdsOf(id)));
        if (FAILED(hr)) throw CameraError(std::format("open {}: {}", usbIdsOf(id), hresultText(hr)));
    }
    ~SystemDevice() override { dev_->Close(); }
    SystemDevice(const SystemDevice&) = delete;
    SystemDevice& operator=(const SystemDevice&) = delete;

    Values send(Command c, const Values& in) override {
        ensureCom();
        auto p = newValues();
        p->SetGuidValue(kCommandCategory, kMtpExt);
        p->SetUnsignedIntegerValue(kCommandId, static_cast<ULONG>(c));
        if (in.opCode) p->SetUnsignedIntegerValue(kOpCode, *in.opCode);
        if (c == Command::ExecuteNoData || c == Command::ExecuteRead || c == Command::ExecuteWrite) {
            Com<IPortableDevicePropVariantCollection> params;
            HRESULT hr = CoCreateInstance(kClsidPropVariants, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IPortableDevicePropVariantCollection),
                                          params.putVoid());
            if (FAILED(hr)) throw Error(std::format("WPD: create a parameter list: {}", hresultText(hr)));
            for (uint32_t x : in.params) {
                PROPVARIANT v;
                PropVariantInit(&v);
                v.vt = VT_UI4;
                v.ulVal = x;
                params->Add(&v);
            }
            p->SetIPortableDevicePropVariantCollectionValue(kOpParams, params.get());
        }
        if (!in.context.empty()) p->SetStringValue(kContext, wide(in.context).c_str());
        if (in.totalSize) p->SetUnsignedLargeIntegerValue(kTotal, *in.totalSize);
        std::vector<BYTE> buffer;
        if (c == Command::ReadData) {
            p->SetUnsignedIntegerValue(kToRead, in.bytes.value_or(0));
            buffer.resize(in.bytes.value_or(0));
            p->SetBufferValue(kData, buffer.data(), static_cast<DWORD>(buffer.size()));
        }
        if (c == Command::WriteData) {
            p->SetUnsignedIntegerValue(kToWrite, in.bytes.value_or(static_cast<uint32_t>(in.data.size())));
            buffer.assign(in.data.begin(), in.data.end());
            p->SetBufferValue(kData, buffer.data(), static_cast<DWORD>(buffer.size()));
        }

        Com<IPortableDeviceValues> r;
        const HRESULT hr = dev_->SendCommand(0, p.get(), r.put());
        if (FAILED(hr)) throw LinkError(std::format("WPD command {}: {}", static_cast<uint32_t>(c), hresultText(hr)));
        Values out;
        HRESULT result = S_OK;
        if (SUCCEEDED(r->GetErrorValue(kHresult, &result))) out.hresult = result;
        ULONG u = 0;
        if (SUCCEEDED(r->GetUnsignedIntegerValue(kRespCode, &u))) out.responseCode = u;
        Com<IPortableDevicePropVariantCollection> list;
        if (SUCCEEDED(r->GetIPortableDevicePropVariantCollectionValue(c == Command::VendorOpcodes ? kVendorOps : kRespParams, list.put())) && list) {
            DWORD n = 0;
            list->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(list->GetAt(i, &v)) && v.vt == VT_UI4) out.params.push_back(v.ulVal);
                PropVariantClear(&v);
            }
        }
        LPWSTR ctx = nullptr;
        if (SUCCEEDED(r->GetStringValue(kContext, &ctx)) && ctx) {
            out.context = utf8(ctx);
            CoTaskMemFree(ctx);
        }
        ULONGLONG total = 0;
        if (SUCCEEDED(r->GetUnsignedLargeIntegerValue(kTotal, &total))) out.totalSize = total;
        if (SUCCEEDED(r->GetUnsignedIntegerValue(kOptimal, &u))) out.optimalBuffer = u;
        if (SUCCEEDED(r->GetUnsignedIntegerValue(c == Command::WriteData ? kWritten : kRead, &u))) out.bytes = u;
        if (c == Command::ReadData) {
            BYTE* b = nullptr;
            DWORD n = 0;
            if (SUCCEEDED(r->GetBufferValue(kData, &b, &n)) && b) {
                out.data.assign(b, b + n);
                CoTaskMemFree(b);
            }
        }
        return out;
    }

private:
    Com<IPortableDevice> dev_;
};

}  // namespace

std::vector<DeviceEntry> listDevices() {
    ensureCom();
    Com<IPortableDeviceManager> m;
    if (FAILED(CoCreateInstance(kClsidManager, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IPortableDeviceManager), m.putVoid()))) return {};
    m->RefreshDeviceList();
    DWORD n = 0;
    if (FAILED(m->GetDevices(nullptr, &n)) || n == 0) return {};
    std::vector<LPWSTR> ids(n, nullptr);
    if (FAILED(m->GetDevices(ids.data(), &n))) return {};
    // the two-call pattern of GetDeviceFriendlyName / Description / Manufacturer
    auto text = [&](LPCWSTR id, HRESULT (STDMETHODCALLTYPE IPortableDeviceManager::*get)(LPCWSTR, WCHAR*, DWORD*)) {
        DWORD len = 0;
        if (FAILED((m.get()->*get)(id, nullptr, &len)) || len == 0) return std::string();
        std::wstring w(len, L'\0');
        if (FAILED((m.get()->*get)(id, w.data(), &len))) return std::string();
        return utf8(w.c_str());
    };
    std::vector<DeviceEntry> out;
    for (DWORD i = 0; i < n; ++i) {
        if (!ids[i]) continue;
        out.push_back({utf8(ids[i]), text(ids[i], &IPortableDeviceManager::GetDeviceFriendlyName),
                       text(ids[i], &IPortableDeviceManager::GetDeviceManufacturer), text(ids[i], &IPortableDeviceManager::GetDeviceDescription)});
        CoTaskMemFree(ids[i]);
    }
    return out;
}

std::unique_ptr<Device> openDevice(const std::string& id) { return std::make_unique<SystemDevice>(id); }

}  // namespace slopcap::ptp::wpd

#pragma once
// Header-only client for Unified Remote 3's uvhid.sys transport.
// Requires Windows, C++17, hid.lib, and setupapi.lib.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace uvhid {

    constexpr std::uint16_t vendor_id = 0x9512;
    constexpr std::uint16_t product_id = 0x9512;
    constexpr std::uint16_t usage_page = 0xFF00;
    constexpr std::uint16_t usage = 0x0001;
    constexpr std::uint8_t transport_report_id = 0x40;
    constexpr std::size_t transport_report_size = 65;
    constexpr std::size_t maximum_payload_size = 62;

    enum class MouseButton : std::uint8_t {
        left   = 0x01,
        right  = 0x02,
        middle = 0x04,
        back   = 0x08,
        forward= 0x10
    };

    struct DeviceInfo {
        std::wstring path;
        std::uint16_t version = 0;
        std::uint16_t output_report_bytes = 0;
    };

    namespace detail {

        inline std::string windows_error(DWORD code) {
            char* text = nullptr;
            const DWORD length = FormatMessageA(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                reinterpret_cast<char*>(&text), 0, nullptr);
            std::string result = length && text
                ? std::string(text, length)
                : "Windows error " + std::to_string(code);
            if (text) LocalFree(text);
            while (!result.empty() && (result.back() == '\r' || result.back() == '\n'))
                result.pop_back();
            return result;
        }

        struct DeviceInfoSetCloser {
            void operator()(void* value) const noexcept {
                if (value && value != INVALID_HANDLE_VALUE)
                    SetupDiDestroyDeviceInfoList(static_cast<HDEVINFO>(value));
            }
        };
        using UniqueDeviceInfoSet = std::unique_ptr<void, DeviceInfoSetCloser>;

inline void append_i16(std::vector<std::uint8_t>& report, std::int16_t value) {
            const auto raw = static_cast<std::uint16_t>(value);
            report.push_back(static_cast<std::uint8_t>(raw));
            report.push_back(static_cast<std::uint8_t>(raw >> 8));
        }

    } // namespace detail

    class Controller {
        public:
            Controller() { open(); }
            explicit Controller(DeviceInfo& info) { open(&info); }

            ~Controller() {
                if (handle_ != INVALID_HANDLE_VALUE)
                    CloseHandle(handle_);
            }

            Controller(const Controller&) = delete;
            Controller& operator=(const Controller&) = delete;

            Controller(Controller&& other) noexcept : handle_(other.handle_) {
                other.handle_ = INVALID_HANDLE_VALUE;
            }

            Controller& operator=(Controller&& other) noexcept {
                if (this != &other) {
                    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
                    handle_ = other.handle_;
                    other.handle_ = INVALID_HANDLE_VALUE;
                }
                return *this;
            }

            void send_report(const std::uint8_t* payload, std::size_t length) {
                if (!payload || length == 0 || length > maximum_payload_size)
                    throw std::invalid_argument(
                        "report must contain 1..62 bytes including its inner report ID");

                std::array<std::uint8_t, transport_report_size> transport{};
                transport[0] = transport_report_id;
                transport[1] = static_cast<std::uint8_t>(length);
                transport[2] = 0; // normal injection command
                std::copy(payload, payload + length, transport.begin() + 3);

                if (!HidD_SetOutputReport(handle_, transport.data(),
                                                    static_cast<ULONG>(transport.size()))) {
                            throw std::runtime_error(
                                "HidD_SetOutputReport: " + detail::windows_error(GetLastError()));
                }
            }

            void send_report(const std::vector<std::uint8_t>& report) {
                send_report(report.data(), report.size());
            }

            // Relative movement. Large deltas are divided into valid signed 8-bit reports.
            void move_mouse(int x, int y) {
                if (x == 0 && y == 0) {
                    mouse_report(0, 0, 0, 0, 0);
                    return;
                }
                while (x != 0 || y != 0) {
                    const int dx = (std::max)(-127, (std::min)(127, x));
                    const int dy = (std::max)(-127, (std::min)(127, y));
                    mouse_report(0, static_cast<std::int8_t>(dx), static_cast<std::int8_t>(dy), 0, 0);
                    x -= dx;
                    y -= dy;
                }
            }

            // Absolute virtual-desktop coordinates in the descriptor's -32767..32767 range.
            void move_mouse_absolute(int x, int y) {
                x = (std::max)(-32767, (std::min)(32767, x));
                y = (std::max)(-32767, (std::min)(32767, y));
                std::vector<std::uint8_t> report{8, held_buttons_};
                detail::append_i16(report, static_cast<std::int16_t>(x));
                detail::append_i16(report, static_cast<std::int16_t>(y));
                report.push_back(0);
                report.push_back(0);
                send_report(report);
            }

            void mouse_down(MouseButton button) {
                held_buttons_ |= static_cast<std::uint8_t>(button);
                mouse_report(held_buttons_, 0, 0, 0, 0);
            }

            void mouse_up(MouseButton button) {
                held_buttons_ &= static_cast<std::uint8_t>(~static_cast<std::uint8_t>(button));
                mouse_report(held_buttons_, 0, 0, 0, 0);
            }

            void click(MouseButton button) {
                mouse_down(button);
                mouse_up(button);
            }

            void left_click()   { click(MouseButton::left); }
            void middle_click() { click(MouseButton::middle); }
            void right_click()  { click(MouseButton::right); }
            void back_click()   { click(MouseButton::back); }
            void forward_click(){ click(MouseButton::forward); }

            void scroll(int vertical, int horizontal = 0) {
                while (vertical != 0 || horizontal != 0) {
                    const int v = (std::max)(-127, (std::min)(127, vertical));
                    const int h = (std::max)(-127, (std::min)(127, horizontal));
                    mouse_report(held_buttons_, 0, 0,
                                 static_cast<std::int8_t>(v), static_cast<std::int8_t>(h));
                    vertical -= v;
                    horizontal -= h;
                }
            }

            // Standard HID keyboard report: modifier mask plus up to six usage IDs.
            void keyboard(std::uint8_t modifiers,
                          const std::vector<std::uint8_t>& usages) {
                if (usages.size() > 6)
                    throw std::invalid_argument("keyboard accepts at most six usages");
                std::vector<std::uint8_t> report{6, modifiers, 0};
                report.insert(report.end(), usages.begin(), usages.end());
                report.resize(9, 0);
                send_report(report);
            }

            void release_keys() { keyboard(0, {}); }
            void consumer(std::uint8_t usage_id) {
                send_report(std::vector<std::uint8_t>{7, usage_id});
            }

        private:
            void open(DeviceInfo* result = nullptr) {
                GUID hid_guid{};
                HidD_GetHidGuid(&hid_guid);
                detail::UniqueDeviceInfoSet devices(
                    SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
                if (devices.get() == INVALID_HANDLE_VALUE)
                    throw std::runtime_error(
                        "SetupDiGetClassDevsW: " + detail::windows_error(GetLastError()));

                for (DWORD index = 0;; ++index) {
                    SP_DEVICE_INTERFACE_DATA interface_data{};
                    interface_data.cbSize = sizeof(interface_data);
                    if (!SetupDiEnumDeviceInterfaces(static_cast<HDEVINFO>(devices.get()), nullptr,
                                                     &hid_guid, index, &interface_data)) {
                        if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
                        continue;
                    }

                    DWORD required = 0;
                    SetupDiGetDeviceInterfaceDetailW(static_cast<HDEVINFO>(devices.get()),
                                                     &interface_data, nullptr, 0, &required, nullptr);
                    if (!required) continue;

                    std::vector<std::uint8_t> storage(required);
                    auto* detail_data =
                        reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
                    detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                    if (!SetupDiGetDeviceInterfaceDetailW(static_cast<HDEVINFO>(devices.get()),
                                                          &interface_data, detail_data, required,
                                                          nullptr, nullptr))
                        continue;

                    HANDLE candidate = CreateFileW(
                        detail_data->DevicePath, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                    if (candidate == INVALID_HANDLE_VALUE) continue;

                    HIDD_ATTRIBUTES attributes{};
                    attributes.Size = sizeof(attributes);
                    PHIDP_PREPARSED_DATA preparsed = nullptr;
                    HIDP_CAPS caps{};
                    const bool matches =
                        HidD_GetAttributes(candidate, &attributes) &&
                        attributes.VendorID == vendor_id &&
                        attributes.ProductID == product_id &&
                        HidD_GetPreparsedData(candidate, &preparsed) &&
                        HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS &&
                        caps.UsagePage == usage_page && caps.Usage == usage;
                    if (preparsed) HidD_FreePreparsedData(preparsed);

                    if (!matches) {
                        CloseHandle(candidate);
                        continue;
                    }

                    handle_ = candidate;
                    if (result) {
                        result->path = detail_data->DevicePath;
                        result->version = attributes.VersionNumber;
                        result->output_report_bytes = caps.OutputReportByteLength;
                    }
                    return;
                }

                throw std::runtime_error(
                    "Unified Virtual HID device not found (9512:9512, FF00:0001)");
            }

        void mouse_report(std::uint8_t buttons, std::int8_t dx, std::int8_t dy,
                            std::int8_t wheel, std::int8_t horizontal_wheel) {
            send_report(std::vector<std::uint8_t>{
                3, buttons, static_cast<std::uint8_t>(dx), static_cast<std::uint8_t>(dy),
                static_cast<std::uint8_t>(wheel),
                static_cast<std::uint8_t>(horizontal_wheel)});
        }

        HANDLE handle_ = INVALID_HANDLE_VALUE;
        std::uint8_t held_buttons_ = 0;
    };

    // Process-wide convenience controller and requested free-function API.
    inline Controller& controller() {
        static Controller instance;
        return instance;
    }

    inline void move_mouse(int x, int y) { controller().move_mouse(x, y); }
    inline void move_mouse_absolute(int x, int y) { controller().move_mouse_absolute(x, y); }
    inline void left_click() { controller().left_click(); }
    inline void middle_click() { controller().middle_click(); }
    inline void right_click() { controller().right_click(); }
    inline void back_click() { controller().back_click(); }
    inline void forward_click() { controller().forward_click(); }
    inline void mouse_down(MouseButton button) { controller().mouse_down(button); }
    inline void mouse_up(MouseButton button) { controller().mouse_up(button); }
    inline void scroll(int vertical, int horizontal = 0) {
        controller().scroll(vertical, horizontal);
    }
    inline void keyboard(std::uint8_t modifiers,
                         const std::vector<std::uint8_t>& usages) {
        controller().keyboard(modifiers, usages);
    }
    inline void release_keys() { controller().release_keys(); }
    inline void consumer(std::uint8_t usage_id) { controller().consumer(usage_id); }

}

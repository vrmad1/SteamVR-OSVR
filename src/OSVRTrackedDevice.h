/** @file
    @brief OSVR tracked device

    @date 2015

    @author
    Sensics, Inc.
    <http://sensics.com/osvr>
*/

// Copyright 2015 Sensics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef INCLUDED_OSVRTrackedDevice_h_GUID_128E3B29_F5FC_4221_9B38_14E3F402E645
#define INCLUDED_OSVRTrackedDevice_h_GUID_128E3B29_F5FC_4221_9B38_14E3F402E645

// Internal Includes
#include "osvr_compiler_detection.h"    // for OSVR_OVERRIDE
#include "Settings.h"
#include "display/Display.h"
#include "PropertyMap.h"
#include "PropertyProperties.h"

// OpenVR includes
#include <openvr_driver.h>

// Library/third-party includes
#include <osvr/ClientKit/Display.h>
#include <osvr/Client/RenderManagerConfig.h>
#include <osvr/RenderKit/DistortionParameters.h>
#include <osvr/RenderKit/UnstructuredMeshInterpolator.h>
#include <osvr/RenderKit/osvr_display_configuration.h>

// Standard includes
#include <string>
#include <memory>
#include <vector>
#include <map>

class OSVRTrackedDevice : public vr::ITrackedDeviceServerDriver {
friend class ServerDriver_OSVR;
public:
    OSVRTrackedDevice(osvr::clientkit::ClientContext& context, vr::IServerDriverHost* driver_host, vr::ETrackedDeviceClass device_class);

    virtual ~OSVRTrackedDevice();

    // ------------------------------------
    // Management Methods
    // ------------------------------------
    /**
     * This is called before an HMD is returned to the application. It will
     * always be called before any display or tracking methods. Memory and
     * processor use by the ITrackedDeviceServerDriver object should be kept to
     * a minimum until it is activated.  The pose listener is guaranteed to be
     * valid until Deactivate is called, but should not be used after that
     * point.
     */
    virtual vr::EVRInitError Activate(uint32_t object_id) OSVR_OVERRIDE;

    /**
     * This is called when The VR system is switching from this Hmd being the
     * active display to another Hmd being the active display. The driver should
     * clean whatever memory and thread use it can when it is deactivated.
     */
    virtual void Deactivate() OSVR_OVERRIDE;

    /**
     * Handles a request from the system to power off this device.
     */
    virtual void PowerOff() OSVR_OVERRIDE;

    /**
     * Requests a component interface of the driver for device-specific
     * functionality. The driver should return NULL if the requested interface
     * or version is not supported.
     */
    virtual void* GetComponent(const char* component_name_and_version) OSVR_OVERRIDE;

    /**
     * A VR Client has made this debug request of the driver. The set of valid
     * requests is entirely up to the driver and the client to figure out, as is
     * the format of the response. Responses that exceed the length of the
     * supplied buffer should be truncated and null terminated.
     */
    virtual void DebugRequest(const char* request, char* response_buffer, uint32_t response_buffer_size) OSVR_OVERRIDE;

    // ------------------------------------
    // Tracking Methods
    // ------------------------------------

    virtual vr::DriverPose_t GetPose() OSVR_OVERRIDE;

    // ------------------------------------
    // Property Methods
    // ------------------------------------

    /**
     * Returns a bool property. If the property is not available this function
     * will return false.
     */
    virtual bool GetBoolTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* error) OSVR_OVERRIDE;

    /**
     * Returns a float property. If the property is not available this function
     * will return 0.
     */
    virtual float GetFloatTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* error) OSVR_OVERRIDE;

    /**
     * Returns an int property. If the property is not available this function
     * will return 0.
     */
    virtual int32_t GetInt32TrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* error) OSVR_OVERRIDE;

    /**
     * Returns a uint64 property. If the property is not available this function
     * will return 0.
     */
    virtual uint64_t GetUint64TrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* error) OSVR_OVERRIDE;

    /**
     * Returns a matrix property. If the device index is not valid or the
     * property is not a matrix type, this function will return identity.
     */
    virtual vr::HmdMatrix34_t GetMatrix34TrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* error) OSVR_OVERRIDE;

    /**
     * Returns a string property. If the property is not available this function
     * will return 0 and @p error will be set to an error. Otherwise it returns
     * the length of the number of bytes necessary to hold this string including
     * the trailing null. If the buffer is too small the error will be
     * @c TrackedProp_BufferTooSmall. Strings will generally fit in buffers of
     * @c k_unTrackingStringSize characters. Drivers may not return strings longer
     * than @c k_unMaxPropertyStringSize.
     */
    virtual uint32_t GetStringTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, char* value, uint32_t buffer_size, vr::ETrackedPropertyError* error) OSVR_OVERRIDE;

protected:
    std::string GetStringTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError *error);

    /**
     * Cecks to see if the requested property is valid for the device class and
     * type requested.
     *
     * @tparam T type of value requested
     * @param prop property requested
     *
     * @return vr::TrackedProp_Success if the checks pass, other
     * vr::ETrackedPropertyError values on failure
     */
    template <typename T>
    vr::ETrackedPropertyError checkProperty(vr::ETrackedDeviceProperty prop, const T&);

    template <typename T>
    T GetTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* error, const T& default_value);

    osvr::clientkit::ClientContext& context_;
    vr::IServerDriverHost* driverHost_ = nullptr;
    vr::DriverPose_t pose_;
    vr::ETrackedDeviceClass deviceClass_;
    std::unique_ptr<Settings> settings_;
    uint32_t objectId_ = 0;

    /** \name Collections of properties and their values. */
    //@{
    PropertyMap properties_;
    //@}
};

template <typename T>
inline T OSVRTrackedDevice::GetTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* error, const T& default_value)
{
    const auto result = checkProperty(prop, T());
    if (vr::TrackedProp_Success != result) {
        if (error)
            *error = result;
        return default_value;
    }

    if (properties_.find(prop) != end(properties_)) {
        if (error)
            *error = vr::TrackedProp_Success;
        return boost::get<T>(properties_[prop]);
    } else {
        if (error)
            *error = vr::TrackedProp_ValueNotProvidedByDevice;
        return default_value;
    }
}

template <typename T>
inline vr::ETrackedPropertyError OSVRTrackedDevice::checkProperty(vr::ETrackedDeviceProperty prop, const T&)
{
    if (isWrongDataType(prop, T())) {
        return vr::TrackedProp_WrongDataType;
    }

    if (isWrongDeviceClass(prop, deviceClass_)) {
        return vr::TrackedProp_WrongDeviceClass;
    }

    if (vr::TrackedDeviceClass_Invalid == deviceClass_) {
        return vr::TrackedProp_InvalidDevice;
    }

    return vr::TrackedProp_Success;
}

#endif // INCLUDED_OSVRTrackedDevice_h_GUID_128E3B29_F5FC_4221_9B38_14E3F402E645


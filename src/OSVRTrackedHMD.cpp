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

// Internal Includes
#include "OSVRTrackedHMD.h"
#include "Logging.h"

#include "osvr_compiler_detection.h"
#include "make_unique.h"
#include "matrix_cast.h"
#include "ValveStrCpy.h"
#include "platform_fixes.h" // strcasecmp
#include "make_unique.h"
#include "osvr_platform.h"
#include "display/DisplayEnumerator.h"

// OpenVR includes
#include <openvr_driver.h>

// Library/third-party includes
#include <osvr/ClientKit/Display.h>
#include <osvr/Util/EigenInterop.h>
#include <osvr/Client/RenderManagerConfig.h>
#include <util/FixedLengthStringFunctions.h>
#include <osvr/RenderKit/DistortionCorrectTextureCoordinate.h>

// Standard includes
#include <cstring>
#include <ctime>
#include <string>
#include <iostream>
#include <exception>
#include <algorithm>        // for std::find

OSVRTrackedHMD::OSVRTrackedHMD(osvr::clientkit::ClientContext& context, vr::IServerDriverHost* driver_host) : OSVRTrackedDevice(context, driver_host, vr::TrackedDeviceClass_HMD)
{
    OSVR_LOG(trace) << "OSVRTrackedHMD::OSVRTrackedHMD() called.";

    settings_ = std::make_unique<Settings>(driver_host->GetSettings(vr::IVRSettings_Version));
    configure();
}

OSVRTrackedHMD::~OSVRTrackedHMD()
{
    // do nothing
}

vr::EVRInitError OSVRTrackedHMD::Activate(uint32_t object_id)
{
    OSVR_LOG(trace) << "OSVRTrackedHMD::Activate() called.";

    OSVRTrackedDevice::Activate(object_id);

    const std::time_t waitTime = 5; // wait up to 5 seconds for init

    // Register tracker callback
    if (trackerInterface_.notEmpty()) {
        trackerInterface_.free();
    }

    // Ensure context is fully started up
    OSVR_LOG(trace) << "OSVRTrackedHMD::Activate(): Waiting for the context to fully start up...\n";
    std::time_t startTime = std::time(nullptr);
    while (!context_.checkStatus()) {
        context_.update();
        if (std::time(nullptr) > startTime + waitTime) {
            OSVR_LOG(err) << "OSVRTrackedHMD::Activate(): Context startup timed out!\n";
            return vr::VRInitError_Driver_Failed;
        }
    }

    configureDistortionParameters();

    displayConfig_ = osvr::clientkit::DisplayConfig(context_);

    // Ensure display is fully started up
    OSVR_LOG(trace) << "OSVRTrackedHMD::Activate(): Waiting for the display to fully start up, including receiving initial pose update...\n";
    startTime = std::time(nullptr);
    while (!displayConfig_.checkStartup()) {
        context_.update();
        if (std::time(nullptr) > startTime + waitTime) {
            OSVR_LOG(err) << "OSVRTrackedHMD::Activate(): Display startup timed out!\n";
            return vr::VRInitError_Driver_Failed;
        }
    }

    // Verify valid display config
    if ((displayConfig_.getNumViewers() != 1) && (displayConfig_.getViewer(0).getNumEyes() != 2) && (displayConfig_.getViewer(0).getEye(0).getNumSurfaces() == 1) && (displayConfig_.getViewer(0).getEye(1).getNumSurfaces() != 1)) {
        OSVR_LOG(err) << "OSVRTrackedHMD::Activate(): Unexpected display parameters!\n";

        if (displayConfig_.getNumViewers() < 1) {
            OSVR_LOG(err) << "OSVRTrackedHMD::Activate(): At least one viewer must exist.\n";
            return vr::VRInitError_Driver_HmdDisplayNotFound;
        } else if (displayConfig_.getViewer(0).getNumEyes() < 2) {
            OSVR_LOG(err) << "OSVRTrackedHMD::Activate(): At least two eyes must exist.\n";
            return vr::VRInitError_Driver_HmdDisplayNotFound;
        } else if ((displayConfig_.getViewer(0).getEye(0).getNumSurfaces() < 1) || (displayConfig_.getViewer(0).getEye(1).getNumSurfaces() < 1)) {
            OSVR_LOG(err) << "OSVRTrackedHMD::Activate(): At least one surface must exist for each eye.\n";
            return vr::VRInitError_Driver_HmdDisplayNotFound;
        }
    }

    // Register tracker callback
    trackerInterface_ = context_.getInterface("/me/head");
    trackerInterface_.registerCallback(&OSVRTrackedHMD::HmdTrackerCallback, this);

    auto configString = context_.getStringParameter("/renderManagerConfig");

    // If the /renderManagerConfig parameter is missing from the configuration
    // file, use an empty dictionary instead. This allows the render manager
    // config to zero out its values.
    if (configString.empty()) {
        OSVR_LOG(info) << "OSVRTrackedHMD::Activate(): Render Manager config is empty, using default values.\n";
        configString = "{}";
    }

    try {
        renderManagerConfig_.parse(configString);
    } catch(const std::exception& e) {
        OSVR_LOG(err) << "OSVRTrackedHMD::Activate(): Exception parsing Render Manager config: " << e.what() << "\n";
    }

    driverHost_->ProximitySensorState(objectId_, true);

    OSVR_LOG(trace) << "OSVRTrackedHMD::Activate(): Activation complete.\n";
    return vr::VRInitError_None;
}

void OSVRTrackedHMD::Deactivate()
{
    OSVR_LOG(trace) << "OSVRTrackedHMD::Deactivate() called.";

    /// Have to force freeing here
    if (trackerInterface_.notEmpty()) {
        trackerInterface_.free();
    }
}

void OSVRTrackedHMD::GetWindowBounds(int32_t* x, int32_t* y, uint32_t* width, uint32_t* height)
{
    int nDisplays = displayConfig_.getNumDisplayInputs();
    if (nDisplays != 1) {
        OSVR_LOG(err) << "OSVRTrackedHMD::OSVRTrackedHMD(): Unexpected display number of displays!\n";
    }
    osvr::clientkit::DisplayDimensions displayDims = displayConfig_.getDisplayDimensions(0);
    *x = renderManagerConfig_.getWindowXPosition(); // todo: assumes desktop display of 1920. get this from display config when it's exposed.
    *y = renderManagerConfig_.getWindowYPosition();
    *width = static_cast<uint32_t>(displayDims.width);
    *height = static_cast<uint32_t>(displayDims.height);

#if defined(OSVR_WINDOWS) || defined(OSVR_MACOSX)
    // ... until we've added code for other platforms
    *x = display_.position.x;
    *y = display_.position.y;
    *height = display_.size.height;
    *width = display_.size.width;
#endif
}

bool OSVRTrackedHMD::IsDisplayOnDesktop()
{
    // If the current display still appeara in the active displays list,
    // then it's attached to the desktop.
    const auto displays = osvr::display::getDisplays();
    const auto display_on_desktop = (end(displays) != std::find(begin(displays), end(displays), display_));
    OSVR_LOG(trace) << "OSVRTrackedHMD::IsDisplayOnDesktop(): " << (display_on_desktop ? "yes" : "no");
    return display_on_desktop;
}

bool OSVRTrackedHMD::IsDisplayRealDisplay()
{
    // TODO get this info from display description?
    return true;
}

void OSVRTrackedHMD::GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height)
{
    /// @todo calculate overfill factor properly
    double overfill_factor = 1.0;
    int32_t x, y;
    uint32_t w, h;
    GetWindowBounds(&x, &y, &w, &h);

    *width = static_cast<uint32_t>(w * overfill_factor);
    *height = static_cast<uint32_t>(h * overfill_factor);
}

void OSVRTrackedHMD::GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* width, uint32_t* height)
{
    osvr::clientkit::RelativeViewport viewPort = displayConfig_.getViewer(0).getEye(eye).getSurface(0).getRelativeViewport();
    *x = static_cast<uint32_t>(viewPort.left);
    *y = static_cast<uint32_t>(viewPort.bottom);
    *width = static_cast<uint32_t>(viewPort.width);
    *height = static_cast<uint32_t>(viewPort.height);
}

void OSVRTrackedHMD::GetProjectionRaw(vr::EVREye eye, float* left, float* right, float* top, float* bottom)
{
    // Reference: https://github.com/ValveSoftware/openvr/wiki/IVRSystem::GetProjectionRaw
    // SteamVR expects top and bottom to be swapped!
    osvr::clientkit::ProjectionClippingPlanes pl = displayConfig_.getViewer(0).getEye(eye).getSurface(0).getProjectionClippingPlanes();
    *left = static_cast<float>(pl.left);
    *right = static_cast<float>(pl.right);
    *bottom = static_cast<float>(pl.top); // SWAPPED
    *top = static_cast<float>(pl.bottom); // SWAPPED
}

vr::DistortionCoordinates_t OSVRTrackedHMD::ComputeDistortion(vr::EVREye eye, float u, float v)
{
    // Note that RenderManager expects the (0, 0) to be the lower-left corner and (1, 1) to be the upper-right corner while SteamVR assumes (0, 0) is upper-left and (1, 1) is lower-right.
    // To accommodate this, we need to flip the y-coordinate before passing it to RenderManager and flip it again before returning the value to SteamVR.
    OSVR_LOG(trace) << "OSVRTrackedHMD::ComputeDistortion(" << eye << ", " << u << ", " << v << ") called.";

    using osvr::renderkit::DistortionCorrectTextureCoordinate;
    static const size_t COLOR_RED = 0;
    static const size_t COLOR_GREEN = 1;
    static const size_t COLOR_BLUE = 2;

    const auto osvr_eye = static_cast<size_t>(eye);
    const auto distortion_parameters = distortionParameters_[osvr_eye];
    const auto in_coords = osvr::renderkit::Float2 {{u, 1.0f - v}}; // flip v-coordinate

    auto interpolators = &leftEyeInterpolators_;
    if (vr::Eye_Right == eye) {
        interpolators = &rightEyeInterpolators_;
    }

    auto coords_red = DistortionCorrectTextureCoordinate(
        osvr_eye, in_coords, distortion_parameters,
        COLOR_RED, overfillFactor_, *interpolators);

    auto coords_green = DistortionCorrectTextureCoordinate(
        osvr_eye, in_coords, distortion_parameters,
        COLOR_GREEN, overfillFactor_, *interpolators);

    auto coords_blue = DistortionCorrectTextureCoordinate(
        osvr_eye, in_coords, distortion_parameters,
        COLOR_BLUE, overfillFactor_, *interpolators);

    vr::DistortionCoordinates_t coords;
    // flip v-coordinates again
    coords.rfRed[0] = coords_red[0];
    coords.rfRed[1] = 1.0f - coords_red[1];
    coords.rfGreen[0] = coords_green[0];
    coords.rfGreen[1] = 1.0f - coords_green[1];
    coords.rfBlue[0] = coords_blue[0];
    coords.rfBlue[1] = 1.0f - coords_blue[1];

    return coords;
}

void OSVRTrackedHMD::HmdTrackerCallback(void* userdata, const OSVR_TimeValue*, const OSVR_PoseReport* report)
{
    if (!userdata)
        return;

    auto* self = static_cast<OSVRTrackedHMD*>(userdata);

    vr::DriverPose_t pose;
    pose.poseTimeOffset = 0; // close enough

    Eigen::Vector3d::Map(pose.vecWorldFromDriverTranslation) = Eigen::Vector3d::Zero();
    Eigen::Vector3d::Map(pose.vecDriverFromHeadTranslation) = Eigen::Vector3d::Zero();

    map(pose.qWorldFromDriverRotation) = Eigen::Quaterniond::Identity();

    map(pose.qDriverFromHeadRotation) = Eigen::Quaterniond::Identity();

    // Position
    Eigen::Vector3d::Map(pose.vecPosition) = osvr::util::vecMap(report->pose.translation);

    // Position velocity and acceleration are not currently consistently provided
    Eigen::Vector3d::Map(pose.vecVelocity) = Eigen::Vector3d::Zero();
    Eigen::Vector3d::Map(pose.vecAcceleration) = Eigen::Vector3d::Zero();

    // Orientation
    map(pose.qRotation) = osvr::util::fromQuat(report->pose.rotation);

    // Angular velocity and acceleration are not currently consistently provided
    Eigen::Vector3d::Map(pose.vecAngularVelocity) = Eigen::Vector3d::Zero();
    Eigen::Vector3d::Map(pose.vecAngularAcceleration) = Eigen::Vector3d::Zero();

    pose.result = vr::TrackingResult_Running_OK;
    pose.poseIsValid = true;
    pose.willDriftInYaw = true;
    pose.shouldApplyHeadModel = true;

    self->pose_ = pose;
    self->driverHost_->TrackedDevicePoseUpdated(self->objectId_, self->pose_);
}

float OSVRTrackedHMD::GetIPD()
{
    OSVR_Pose3 leftEye, rightEye;

    if (displayConfig_.getViewer(0).getEye(0).getPose(leftEye) != true) {
        OSVR_LOG(err) << "OSVRTrackedHMD::GetHeadFromEyePose(): Unable to get left eye pose!\n";
    }

    if (displayConfig_.getViewer(0).getEye(1).getPose(rightEye) != true) {
        OSVR_LOG(err) << "OSVRTrackedHMD::GetHeadFromEyePose(): Unable to get right eye pose!\n";
    }

    float ipd = static_cast<float>((osvr::util::vecMap(leftEye.translation) - osvr::util::vecMap(rightEye.translation)).norm());
    return ipd;
}

void OSVRTrackedHMD::configure()
{
    // The name of the display we want to use
    const std::string display_name = settings_->getSetting<std::string>("displayName", "OSVR");

    // Detect displays and find the one we're using as an HMD
    bool display_found = false;
    auto displays = osvr::display::getDisplays();
    for (const auto& display : displays) {
        if (std::string::npos == display.name.find(display_name))
            continue;

        display_ = display;
        display_found = true;
        break;
    }

    if (!display_found) {
        // Default to OSVR HDK display settings
        display_.adapter.description = "Unknown";
        display_.name = "OSVR HDK";
        display_.size.width = 1920;
        display_.size.height = 1080;
        display_.position.x = 1920;
        display_.position.y = 0;
        display_.rotation = osvr::display::Rotation::Zero;
        display_.verticalRefreshRate = 60.0;
        display_.attachedToDesktop = true;
        display_.edidVendorId = 0xd24e;// 53838
        display_.edidProductId = 0x1019; // 4121
    }

    if (display_found) {
        OSVR_LOG(info) << "Detected display named [" << display_.name << "]:";
    } else {
        OSVR_LOG(info) << "Default display:";
    }
    OSVR_LOG(info) << "  Adapter: " << display_.adapter.description;
    OSVR_LOG(info) << "  Monitor name: " << display_.name;
    OSVR_LOG(info) << "  Resolution: " << display_.size.width << "x" << display_.size.height;
    OSVR_LOG(info) << "  Position: (" << display_.position.x << ", " << display_.position.y << ")";
    switch (display_.rotation) {
    case osvr::display::Rotation::Zero:
        OSVR_LOG(info) << "  Rotation: Landscape";
        break;
    case osvr::display::Rotation::Ninety:
        OSVR_LOG(info) << "  Rotation: Portrait";
        break;
    case osvr::display::Rotation::OneEighty:
        OSVR_LOG(info) << "  Rotation: Landscape (flipped)";
        break;
    case osvr::display::Rotation::TwoSeventy:
        OSVR_LOG(info) << "  Rotation: Portrait (flipped)";
        break;
    default:
        OSVR_LOG(info) << "  Rotation: Landscape";
        break;
    }
    OSVR_LOG(info) << "  Refresh rate: " << display_.verticalRefreshRate;
    OSVR_LOG(info) << "  " << (display_.attachedToDesktop ? "Extended mode" : "Direct mode");
    OSVR_LOG(info) << "  EDID vendor ID: " << display_.edidVendorId;
    OSVR_LOG(info) << "  EDID product ID: " << display_.edidProductId;
}

void OSVRTrackedHMD::configureDistortionParameters()
{
    // Parse the display descriptor
    displayDescription_ = context_.getStringParameter("/display");
    displayConfiguration_ = OSVRDisplayConfiguration(displayDescription_);

    // Initialize the distortion parameters
    OSVR_LOG(debug) << "OSVRTrackedHMD::configureDistortionParameters(): Number of eyes: " << displayConfiguration_.getEyes().size() << ".";
    for (size_t i = 0; i < displayConfiguration_.getEyes().size(); ++i) {
        auto distortion = osvr::renderkit::DistortionParameters { displayConfiguration_, i };
        distortion.m_desiredTriangles = 200 * 64;
        OSVR_LOG(debug) << "OSVRTrackedHMD::configureDistortionParameters(): Adding distortion for eye " << i << ".";
        distortionParameters_.push_back(distortion);
    }
    OSVR_LOG(debug) << "OSVRTrackedHMD::configureDistortionParameters(): Number of distortion parameters: " << distortionParameters_.size() << ".";

    // Make the interpolators to be used by each eye.
    OSVR_LOG(debug) << "OSVRTrackedHMD::configureDistortionParameters(): Creating mesh interpolators for the left eye.";
    if (!makeUnstructuredMeshInterpolators(distortionParameters_[0], 0, leftEyeInterpolators_)) {
        OSVR_LOG(err) << "OSVRTrackedHMD::configureDistortionParameters(): Could not create mesh interpolators for left eye.";
    }
    OSVR_LOG(debug) << "OSVRTrackedHMD::configureDistortionParameters(): Number of left eye interpolators: " << leftEyeInterpolators_.size() << ".";

    OSVR_LOG(debug) << "OSVRTrackedHMD::configureDistortionParameters(): Creating mesh interpolators for the right eye.";
    if (!makeUnstructuredMeshInterpolators(distortionParameters_[1], 1, rightEyeInterpolators_)) {
        OSVR_LOG(err) << "OSVRTrackedHMD::configureDistortionParameters(): Could not create mesh interpolators for right eye.";
    }
    OSVR_LOG(debug) << "OSVRTrackedHMD::configureDistortionParameters(): Number of right eye interpolators: " << leftEyeInterpolators_.size() << ".";
}

void OSVRTrackedHMD::configureProperties()
{
    // General properties that apply to all device classes

    properties_[vr::Prop_WillDriftInYaw_Bool] = true;
    properties_[vr::Prop_DeviceIsWireless_Bool] = false;
    properties_[vr::Prop_DeviceIsCharging_Bool] = false;
    properties_[vr::Prop_Firmware_UpdateAvailable_Bool] = false;
    properties_[vr::Prop_Firmware_ManualUpdate_Bool] = false;
    properties_[vr::Prop_BlockServerShutdown_Bool] = false;
    //properties_[vr::Prop_CanUnifyCoordinateSystemWithHmd_Bool] = true;
    properties_[vr::Prop_ContainsProximitySensor_Bool] = false;
    properties_[vr::Prop_DeviceProvidesBatteryStatus_Bool] = false;
    properties_[vr::Prop_DeviceCanPowerOff_Bool] = true;
    properties_[vr::Prop_HasCamera_Bool] = false;

    properties_[vr::Prop_DeviceBatteryPercentage_Float] = 1.0f; // full battery

    properties_[vr::Prop_DeviceClass_Int32] = deviceClass_;

    //properties_[vr::Prop_HardwareRevision_Uint64] = 0ul;
    //properties_[vr::Prop_FirmwareVersion_Uint64] = 0ul;
    //properties_[vr::Prop_FPGAVersion_Uint64] = 0ul;
    //properties_[vr::Prop_VRCVersion_Uint64] = 0ul;
    //properties_[vr::Prop_RadioVersion_Uint64] = 0ul;
    //properties_[vr::Prop_DongleVersion_Uint64] = 0ul;

    //properties_[vr::Prop_StatusDisplayTransform_Matrix34] = /* TODO */

    //properties_[vr::Prop_TrackingSystemName_String] = "";
    properties_[vr::Prop_ModelNumber_String] = "OSVR HMD";
    properties_[vr::Prop_SerialNumber_String] = display_.name;
    //properties_[vr::Prop_RenderModelName_String] = "";
    //properties_[vr::Prop_ManufacturerName_String] = "";
    //properties_[vr::Prop_TrackingFirmwareVersion_String] = "";
    //properties_[vr::Prop_HardwareRevision_String] = "";
    //properties_[vr::Prop_AllWirelessDongleDescriptions_String] = "";
    //properties_[vr::Prop_ConnectedWirelessDongle_String] = "";
    //properties_[vr::Prop_Firmware_ManualUpdateURL_String] = "";
    //properties_[vr::Prop_Firmware_ProgrammingTarget_String] = "";
    //properties_[vr::Prop_DriverVersion_String] = "";


    // Properties that apply to HMDs

    //properties_[vr::Prop_ReportsTimeSinceVSync_Bool] = false;
    properties_[vr::Prop_IsOnDesktop_Bool] = IsDisplayOnDesktop();

    //properties_[vr::Prop_SecondsFromVsyncToPhotons_Float] = 0.0;
    properties_[vr::Prop_DisplayFrequency_Float] = static_cast<float>(display_.verticalRefreshRate);
    properties_[vr::Prop_UserIpdMeters_Float] = GetIPD();
    //properties_[vr::Prop_DisplayMCOffset_Float] = 0.0;
    //properties_[vr::Prop_DisplayMCScale_Float] = 0.0;
    //properties_[vr::Prop_DisplayGCBlackClamp_Float] = 0.0;
    //properties_[vr::Prop_DisplayGCOffset_Float] = 0.0;
    //properties_[vr::Prop_DisplayGCScale_Float] = 0.0;
    //properties_[vr::Prop_DisplayGCPrescale_Float] = 0.0;
    //properties_[vr::Prop_LensCenterLeftU_Float] = 0.0;
    //properties_[vr::Prop_LensCenterLeftV_Float] = 0.0;
    //properties_[vr::Prop_LensCenterRightU_Float] = 0.0;
    //properties_[vr::Prop_LensCenterRightV_Float] = 0.0;
    //properties_[vr::Prop_UserHeadToEyeDepthMeters_Float] = 0.0;

    //properties_[vr::Prop_DisplayMCType_Int32] = 0;
    properties_[vr::Prop_EdidVendorID_Int32] = static_cast<int32_t>(display_.edidVendorId);
    properties_[vr::Prop_EdidProductID_Int32] = static_cast<int32_t>(display_.edidProductId);
    //properties_[vr::Prop_DisplayGCType_Int32] = 0;
    //properties_[vr::Prop_CameraCompatibilityMode_Int32] = 0;

    properties_[vr::Prop_CurrentUniverseId_Uint64] = 1;
    properties_[vr::Prop_PreviousUniverseId_Uint64] = 1;
    properties_[vr::Prop_DisplayFirmwareVersion_Uint64] = 192; // FIXME read from OSVR server
    //properties_[vr::Prop_CameraFirmwareVersion_Uint64] = 0ul;
    //properties_[vr::Prop_DisplayFPGAVersion_Uint64] = 0ul;
    //properties_[vr::Prop_DisplayBootloaderVersion_Uint64] = 0ul;
    //properties_[vr::Prop_DisplayHardwareVersion_Uint64] = 0ul;
    //properties_[vr::Prop_AudioFirmwareVersion_Uint64] = 0ul;

    //properties_[vr::Prop_CameraToHeadTransform_Matrix34] = /* TODO */

    //properties_[vr::Prop_DisplayMCImageLeft_String] = "";
    //properties_[vr::Prop_DisplayMCImageRight_String] = "";
    //properties_[vr::Prop_DisplayGCImage_String] = "";
    //properties_[vr::Prop_CameraFirmwareDescription_String] = "";
}


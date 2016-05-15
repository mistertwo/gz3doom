/*
** gl_openvr.cpp
** Stereoscopic virtual reality mode for the HTC Vive headset
**
**---------------------------------------------------------------------------
** Copyright 2016 Christopher Bruns
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include "gl/stereo3d/gl_openvr.h"
#include "openvr.h"
#include <string>
#include "doomtype.h" // Printf
#include "g_game.h" // G_Add...
#include "r_utility.h" // viewpitch
#include "gl/renderer/gl_renderer.h"
#include "gl/system/gl_system.h"
#include "c_cvars.h"
#include "LSMatrix.h"

EXTERN_CVAR(Int, screenblocks);

using namespace vr;

namespace s3d 
{

static HmdVector3d_t eulerAnglesFromQuat(HmdQuaternion_t quat) {
	double q0 = quat.w;
	// permute axes to make "Y" up/yaw
	double q2 = quat.x;
	double q3 = quat.y;
	double q1 = quat.z;

	// http://stackoverflow.com/questions/18433801/converting-a-3x3-matrix-to-euler-tait-bryan-angles-pitch-yaw-roll
	double roll = atan2(2 * (q0*q1 + q2*q3), 1 - 2 * (q1*q1 + q2*q2));
	double pitch = asin(2 * (q0*q2 - q3*q1));
	double yaw = atan2(2 * (q0*q3 + q1*q2), 1 - 2 * (q2*q2 + q3*q3));

	return HmdVector3d_t{ yaw, pitch, roll };
}

static HmdQuaternion_t quatFromMatrix(HmdMatrix34_t matrix) {
	HmdQuaternion_t q;
	typedef float f34[3][4];
	f34& a = matrix.m;
	// http://www.euclideanspace.com/maths/geometry/rotations/conversions/matrixToQuaternion/
	float trace = a[0][0] + a[1][1] + a[2][2]; // I removed + 1.0f; see discussion with Ethan
	if (trace > 0) {// I changed M_EPSILON to 0
		float s = 0.5f / sqrtf(trace + 1.0f);
		q.w = 0.25f / s;
		q.x = (a[2][1] - a[1][2]) * s;
		q.y = (a[0][2] - a[2][0]) * s;
		q.z = (a[1][0] - a[0][1]) * s;
	}
	else {
		if (a[0][0] > a[1][1] && a[0][0] > a[2][2]) {
			float s = 2.0f * sqrtf(1.0f + a[0][0] - a[1][1] - a[2][2]);
			q.w = (a[2][1] - a[1][2]) / s;
			q.x = 0.25f * s;
			q.y = (a[0][1] + a[1][0]) / s;
			q.z = (a[0][2] + a[2][0]) / s;
		}
		else if (a[1][1] > a[2][2]) {
			float s = 2.0f * sqrtf(1.0f + a[1][1] - a[0][0] - a[2][2]);
			q.w = (a[0][2] - a[2][0]) / s;
			q.x = (a[0][1] + a[1][0]) / s;
			q.y = 0.25f * s;
			q.z = (a[1][2] + a[2][1]) / s;
		}
		else {
			float s = 2.0f * sqrtf(1.0f + a[2][2] - a[0][0] - a[1][1]);
			q.w = (a[1][0] - a[0][1]) / s;
			q.x = (a[0][2] + a[2][0]) / s;
			q.y = (a[1][2] + a[2][1]) / s;
			q.z = 0.25f * s;
		}
	}

	return q;
}

static HmdVector3d_t eulerAnglesFromMatrix(HmdMatrix34_t mat) {
	return eulerAnglesFromQuat(quatFromMatrix(mat));
}


/* static */
const OpenVRMode& OpenVRMode::getInstance()
{
	static OpenVRMode instance;
	return instance;
}


OpenVREyePose::OpenVREyePose(vr::EVREye eye)
	: ShiftedEyePose( 0.0f )
	, eye(eye)
	, eyeTexture(nullptr)
	, currentPose(nullptr)
{
}


/* virtual */
OpenVREyePose::~OpenVREyePose() 
{
	dispose();
}


/* virtual */
GL_IRECT* OpenVREyePose::GetViewportBounds(GL_IRECT* bounds) const
{
	static GL_IRECT viewportBounds;
	viewportBounds.left = 0;
	viewportBounds.top = 0;
	viewportBounds.width = framebuffer.getWidth();
	viewportBounds.height = framebuffer.getHeight();
	return &viewportBounds;
}


static void vSMatrixFromHmdMatrix34(VSMatrix& m1, const vr::HmdMatrix34_t& m2)
{
	float tmp[16];
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 4; ++j) {
			tmp[4 * i + j] = m2.m[i][j];
		}
	}
	int i = 3;
	for (int j = 0; j < 4; ++j) {
		tmp[4 * i + j] = 0;
	}
	tmp[15] = 1;
	m1.loadMatrix(&tmp[0]);
}


/* virtual */
void OpenVREyePose::GetViewShift(FLOATTYPE yaw, FLOATTYPE outViewShift[3]) const
{
	if (currentPose == nullptr)
		return;
	const vr::TrackedDevicePose_t& hmd = *currentPose;
	if (! hmd.bDeviceIsConnected)
		return;
	if (! hmd.bPoseIsValid)
		return;
	const vr::HmdMatrix34_t& hmdPose = hmd.mDeviceToAbsoluteTracking;

	// Pitch and Roll are identical between OpenVR and Doom worlds.
	// But yaw can differ, depending on starting state, and controller movement.
	float doomYawDegrees = GLRenderer->mAngles.Yaw.Degrees;
	float openVrYawDegrees = -eulerAnglesFromMatrix(hmdPose).v[0] * 180.0 / 3.14159;
	float deltaYawDegrees = doomYawDegrees - openVrYawDegrees;
	while (deltaYawDegrees > 180)
		deltaYawDegrees -= 360;
	while (deltaYawDegrees < -180)
		deltaYawDegrees += 360;

	// First test, just get stereoscopic shift, not position shift
	LSMatrix44 hmdLs(hmdPose);
	LSMatrix44 hmdRot = hmdLs.getWithoutTranslation().transpose();

	LSMatrix44 eyeShift2;
	eyeShift2 = eyeShift2 * eyeToHeadTransform; // eye to head
	eyeShift2 = eyeShift2 * hmdRot; // head to openvr
	eyeShift2.rotate(-deltaYawDegrees, 0, 1, 0); // openvr to doom
	// LSMatrix44 eyeShift = eyeToHeadTransform * hmdRot;

	LSMatrix44 origShift;
	origShift.multMatrix(eyeToHeadTransform);
	float origShiftX = origShift[0][3];

	float doomUnitsPerMeter = 32.0f;
	outViewShift[0] = eyeShift2[0][3] * doomUnitsPerMeter;
	outViewShift[1] = -eyeShift2[2][3] * doomUnitsPerMeter;
	outViewShift[2] = -eyeShift2[1][3] * doomUnitsPerMeter; // TODO: sign here?

	if (eye == vr::Eye_Left) {
		Printf("dYaw = %.1f yaw = %.1f doomShift = %.1f, %.1f\n", 
			deltaYawDegrees,
			GLRenderer->mAngles.Yaw.Degrees,
			eyeShift2[0][3] * doomUnitsPerMeter,
			-eyeShift2[2][3] * doomUnitsPerMeter);
	}
}

/* virtual */
VSMatrix OpenVREyePose::GetProjection(FLOATTYPE fov, FLOATTYPE aspectRatio, FLOATTYPE fovRatio) const
{
	// Ignore those arguments and get the projection from the SDK
	VSMatrix vs1 = ShiftedEyePose::GetProjection(fov, aspectRatio, fovRatio);
	return projectionMatrix;
}


/* virtual */
void OpenVREyePose::SetUp() const
{
	super::SetUp();

	// bind framebuffer
	framebuffer.bindRenderBuffer();

	// TODO: just for testing
	glClearColor(1, 0.5f, 0.5f, 0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* virtual */
void OpenVREyePose::TearDown() const
{
	// submitFrame();
	super::TearDown();
}

void OpenVREyePose::initialize(vr::IVRSystem& vrsystem)
{
	float zNear = 5.0;
	float zFar = 65536.0;
	vr::HmdMatrix44_t projection = vrsystem.GetProjectionMatrix(
			eye, zNear, zFar, vr::API_OpenGL);
	vr::HmdMatrix44_t proj_transpose;
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			proj_transpose.m[i][j] = projection.m[j][i];
		}
	}
	projectionMatrix.loadIdentity();
	projectionMatrix.multMatrix(&proj_transpose.m[0][0]);


	vr::HmdMatrix34_t eyeToHead = vrsystem.GetEyeToHeadTransform(eye);
	vSMatrixFromHmdMatrix34(eyeToHeadTransform, eyeToHead);

	uint32_t w, h;
	vrsystem.GetRecommendedRenderTargetSize(&w, &h);

	// create frame buffers
	framebuffer.initialize(w, h);

	if (eyeTexture == nullptr)
		eyeTexture = new vr::Texture_t();
	eyeTexture->handle = (void*)framebuffer.getRenderTextureId(); // TODO: use resolveTextureId, after I implement warping
	eyeTexture->eType = vr::API_OpenGL;
	eyeTexture->eColorSpace = vr::ColorSpace_Gamma;
}


void OpenVREyePose::dispose()
{
	framebuffer.dispose();
	delete eyeTexture;
	eyeTexture = nullptr;
}


bool OpenVREyePose::submitFrame() const
{
	if (eyeTexture == nullptr)
		return false;
	vr::VRCompositor()->Submit(eye, eyeTexture);
	return true;
}


OpenVRMode::OpenVRMode() 
	: ivrSystem(nullptr)
	, leftEyeView(vr::Eye_Left)
	, rightEyeView(vr::Eye_Right)
{
	eye_ptrs.Push(&leftEyeView); // default behavior to Mono non-stereo rendering

	EVRInitError eError;
	if (VR_IsHmdPresent())
	{
		ivrSystem = VR_Init(&eError, VRApplication_Scene);
		if (eError != vr::VRInitError_None) {
			std::string errMsg = VR_GetVRInitErrorAsEnglishDescription(eError);
			ivrSystem = nullptr;
			return;
			// TODO: report error
		}
		// OK
		leftEyeView.initialize(*ivrSystem);
		rightEyeView.initialize(*ivrSystem);

		if (!vr::VRCompositor())
			return;

		eye_ptrs.Push(&rightEyeView); // NOW we render to two eyes
	}
}


void OpenVRMode::updateDoomViewDirection() const
{
	if (ivrSystem == nullptr)
		return;
	// Compute how far in the future to predict HMD pose
	float secondsSinceLastVsync = 0;
	ivrSystem->GetTimeSinceLastVsync(&secondsSinceLastVsync, nullptr);
	float displayFrequency = ivrSystem->GetFloatTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, Prop_DisplayFrequency_Float);
	float frameDuration = 1.0f / displayFrequency;
	float vsyncToPhotons = ivrSystem->GetFloatTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, Prop_SecondsFromVsyncToPhotons_Float);
	float predictedSecondsFromNow = frameDuration - secondsSinceLastVsync + vsyncToPhotons;

	// Compute HMD pose, in terms of yaw, pitch, and roll
	TrackedDevicePose_t trackedDevicePoses[k_unMaxTrackedDeviceCount];
	ivrSystem->GetDeviceToAbsoluteTrackingPose(
		TrackingUniverseStanding,
		predictedSecondsFromNow,
		trackedDevicePoses,
		k_unMaxTrackedDeviceCount
	);
	TrackedDevicePose_t& hmdPose = trackedDevicePoses[k_unTrackedDeviceIndex_Hmd];

	HmdVector3d_t eulerAngles = eulerAnglesFromMatrix(hmdPose.mDeviceToAbsoluteTracking);
	// Printf("%.1f %.1f %.1f\n", eulerAngles.v[0], eulerAngles.v[1], eulerAngles.v[2]);

	if (hmdPose.bPoseIsValid)
		updateHmdPose(eulerAngles.v[0], eulerAngles.v[1], eulerAngles.v[2]);
}


void OpenVRMode::updateHmdPose(
	double hmdYawRadians, 
	double hmdPitchRadians, 
	double hmdRollRadians) const 
{
	double hmdyaw = hmdYawRadians;
	double hmdpitch = hmdPitchRadians;
	double hmdroll = hmdRollRadians;

	// Set HMD angle game state parameters for NEXT frame
	static double previousYaw = 0;
	static bool havePreviousYaw = false;
	if (!havePreviousYaw) {
		previousYaw = hmdyaw;
		havePreviousYaw = true;
	}
	double dYaw = hmdyaw - previousYaw;
	G_AddViewAngle((int)(-32768.0*dYaw / 3.14159)); // determined empirically
	previousYaw = hmdyaw;

	// Pitch
	int pitch = (int)(-32768 / 3.14159*hmdpitch);
	int dPitch = (pitch - viewpitch / 65536); // empirical
	G_AddViewPitch(-dPitch);

	// Roll can be local, because it doesn't affect gameplay.
	GLRenderer->mAngles.Roll = -hmdroll * 180.0 / 3.14159;

	// Late-schedule update to renderer angles directly, too
	GLRenderer->mAngles.Pitch = -hmdpitch * 180.0 / 3.14159;
	GLRenderer->mAngles.Yaw += dYaw * 180.0 / 3.14159; // TODO: Is this correct? Maybe minus?
}

/* virtual */
void OpenVRMode::SetUp() const
{
	super::SetUp();

	cachedScreenBlocks = screenblocks;
	screenblocks = 12; // always be full-screen during 3D scene render

	if (vr::VRCompositor() == nullptr)
		return;

	static vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
	vr::VRCompositor()->WaitGetPoses(
		poses, vr::k_unMaxTrackedDeviceCount, // current pose
		nullptr, 0 // future pose?
	);

	TrackedDevicePose_t& hmdPose = poses[vr::k_unTrackedDeviceIndex_Hmd];

	if (hmdPose.bPoseIsValid) {
		HmdVector3d_t eulerAngles = eulerAnglesFromMatrix(hmdPose.mDeviceToAbsoluteTracking);
		// Printf("%.1f %.1f %.1f\n", eulerAngles.v[0], eulerAngles.v[1], eulerAngles.v[2]);
		updateHmdPose(eulerAngles.v[0], eulerAngles.v[1], eulerAngles.v[2]);
		leftEyeView.setCurrentHmdPose(&hmdPose);
		rightEyeView.setCurrentHmdPose(&hmdPose);
	}
}

/* virtual */
void OpenVRMode::TearDown() const
{
	screenblocks = cachedScreenBlocks;

	// Unbind eye texture framebuffer
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	leftEyeView.submitFrame();
	rightEyeView.submitFrame();
	glFinish();

	// TODO: bind Hud framebuffer

	super::TearDown();
}

/* virtual */
OpenVRMode::~OpenVRMode() 
{
	if (ivrSystem != nullptr) {
		VR_Shutdown();
		ivrSystem = nullptr;
		leftEyeView.dispose();
		rightEyeView.dispose();
	}
}

} /* namespace s3d */

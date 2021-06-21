LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    AudioPolicyService.cpp \
    AudioPolicyEffects.cpp \
    AudioPolicyInterfaceImpl.cpp \
    AudioPolicyClientImpl.cpp \
    CaptureStateNotifier.cpp

LOCAL_C_INCLUDES := \
    frameworks/av/services/audioflinger \
    $(call include-path-for, audio-utils)

LOCAL_HEADER_LIBRARIES := \
    libaudiopolicycommon \
    libaudiopolicyengine_interface_headers \
    libaudiopolicymanager_interface_headers

LOCAL_SHARED_LIBRARIES := \
    libactivitymanager_aidl \
    libcutils \
    libutils \
    liblog \
    libbinder \
    libaudioclient \
    libaudioutils \
    libaudiofoundation \
    libhardware_legacy \
    libaudiopolicymanager \
    libmedia_helper \
    libmediametrics \
    libmediautils \
    libeffectsconfig \
    libsensorprivacy \
    capture_state_listener-aidl-cpp

LOCAL_EXPORT_SHARED_LIBRARY_HEADERS := \
    libactivitymanager_aidl \
    libsensorprivacy

LOCAL_STATIC_LIBRARIES := \
    libaudiopolicycomponents

LOCAL_MODULE:= libaudiopolicyservice
LOCAL_LICENSE_KINDS:= SPDX-license-identifier-Apache-2.0
LOCAL_LICENSE_CONDITIONS:= notice
LOCAL_NOTICE_FILE:= $(LOCAL_PATH)/../../../NOTICE

LOCAL_CFLAGS += -fvisibility=hidden
LOCAL_CFLAGS += -Wall -Werror -Wthread-safety

include $(BUILD_SHARED_LIBRARY)

/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//--------------------------------------------------------------------------------------
// File: NGFX_Types.h
//
// Distributed as part of the NVIDIA Nsight Graphics SDK.
//--------------------------------------------------------------------------------------

#ifndef NGFX_TYPES_H
#define NGFX_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup NGFX_API_CORE
 * @{
 */

/**
 * @cond INTERNAL_IMPLEMENTATION_DETAILS
 */
#define NGFX_MAKE_STRUCT_VERSION(typeName, ver) (uint32_t)(sizeof(typeName) | ((ver) << 16))
#define NGFX_GET_STRUCT_VERSION(ver) (uint32_t)((ver) >> 16)
#define NGFX_GET_STRUCT_SIZE(ver) (uint32_t)((ver) & 0xffff)
/**
 * @endcond
 */

#if defined(_WIN32)
typedef wchar_t NGFX_PathChar;
#define NGFX_LITERAL(x) L##x
#else
typedef char NGFX_PathChar;
#define NGFX_LITERAL(x) x
#endif

/**
 * @brief Typedef for a callback function for library loading
 *
 * Clients must specify the load library function that they choose to use
 */
typedef void* (*NGFX_LoadLibraryFnPtr)(const NGFX_PathChar* libName);

/**
 * @brief Enum representing the type of activity made available by Nsight Graphics.
 *
 * Given the different needs of activities, there are activity-specific methods
 * and activity-specific initialization that is required.
 */
typedef enum NGFX_ActivityType
{
    NGFX_ActivityType_GraphicsCapture = 0, /**< Graphics Capture activity type. */
    NGFX_ActivityType_GPUTrace = 1,        /**< GPU Trace activity type. */
    NGFX_ActivityType_SystemProfiling = 2, /**< System Profiling activity type. */
    NGFX_ActivityType_COUNT,               /**< Total count of activity types. */
} NGFX_ActivityType;

/**
 * @brief Enum representing the API of the activity that will be analyzed
 *
 * Given the different needs of activities, there are activity-specific methods
 * and activity-specific initialization that is required.
 */
typedef enum NGFX_ApiType
{
    NGFX_ApiType_D3D12 = 0,  /**< D3D12 applications. */
    NGFX_ApiType_Vulkan = 1, /**< Vulkan applications. */
    NGFX_ApiType_CUDA = 2,   /**< CUDA applications. */
    NGFX_ApiType_CUDART = 3, /**< CUDA runtime applications. */
    NGFX_ApiType_OpenGL = 4, /**< OpenGL applications. */
    NGFX_ApiType_COUNT,      /**< Total count of APIs types. */
} NGFX_ApiType;

/**
 * @brief Enum representing the result of an API operation.
 *
 * Most functions within the NGFX Injection API will report a result code that
 * identifies the success or failure of the operation.
 */
typedef enum NGFX_Result
{
    NGFX_Result_Success,                   /**< Operation completed successfully. */
    NGFX_Result_NotImplemented,            /**< The requested operation is not implemented. */
    NGFX_Result_LibNotFound,               /**< The requested library could not be found. */
    NGFX_Result_InvalidLib,                /**< An invalid library was specified. */
    NGFX_Result_DifferentActivityInjected, /**< A different activity is already injected. */
    NGFX_Result_InvalidParameter,          /**< An invalid parameter was passed to the API. */
    NGFX_Result_InvalidState,              /**< The operation cannot complete due to the current state of the API. */
    NGFX_Result_UnspecifiedError,          /**< The requested operation could not be completed. */
    NGFX_Result_Timeout,                   /**< The operation timed out. */
    NGFX_Result_InsufficientBuffer,        /**< The supplied output buffer is too small. */
    NGFX_Result_COUNT,                     /**< Total count of result codes. */
} NGFX_Result;

/**
 * @brief Parameters for querying the number of completed activity artifact files.
 */
typedef struct NGFX_ArtifactFileCount_Params_V1
{
    uint32_t version; /**< Used for versioning; must be assigned a value of NGFX_ArtifactFileCount_Params_VER. */

    uint32_t count; /**< Number of successfully completed artifact files. */
} NGFX_ArtifactFileCount_Params_V1;
typedef NGFX_ArtifactFileCount_Params_V1 NGFX_ArtifactFileCount_Params;
#define NGFX_ArtifactFileCount_Params_V1_VER NGFX_MAKE_STRUCT_VERSION(NGFX_ArtifactFileCount_Params_V1, 1)
#define NGFX_ArtifactFileCount_Params_VER NGFX_ArtifactFileCount_Params_V1_VER

/**
 * @brief Parameters for retrieving the path of a completed activity artifact file.
 */
typedef struct NGFX_ArtifactFilePath_Params_V1
{
    uint32_t version; /**< Used for versioning; must be assigned a value of NGFX_ArtifactFilePath_Params_VER. */

    uint32_t artifactIndex;        /**< Zero-based index of the completed artifact file. */
    NGFX_PathChar* filePath;       /**< Caller-owned output buffer for the null-terminated artifact path. */
    uint32_t filePathCapacity;     /**< Capacity of filePath in NGFX_PathChar elements. */
    uint32_t requiredPathCapacity; /**< Required capacity, including the null terminator. */
} NGFX_ArtifactFilePath_Params_V1;
typedef NGFX_ArtifactFilePath_Params_V1 NGFX_ArtifactFilePath_Params;
#define NGFX_ArtifactFilePath_Params_V1_VER NGFX_MAKE_STRUCT_VERSION(NGFX_ArtifactFilePath_Params_V1, 1)
#define NGFX_ArtifactFilePath_Params_VER NGFX_ArtifactFilePath_Params_V1_VER

/**
 * @brief Parameters for waiting for and retrieving a completed activity artifact file path.
 */
typedef struct NGFX_WaitForArtifactFilePath_Params_V1
{
    uint32_t version; /**< Used for versioning; must be assigned a value of NGFX_WaitForArtifactFilePath_Params_VER. */

    uint32_t artifactIndex;        /**< Zero-based index of the completed artifact file. */
    int32_t timeoutMs;             /**< Timeout in milliseconds. A negative value waits indefinitely. */
    NGFX_PathChar* filePath;       /**< Caller-owned output buffer for the null-terminated artifact path. */
    uint32_t filePathCapacity;     /**< Capacity of filePath in NGFX_PathChar elements. */
    uint32_t requiredPathCapacity; /**< Required capacity, including the null terminator. */
} NGFX_WaitForArtifactFilePath_Params_V1;
typedef NGFX_WaitForArtifactFilePath_Params_V1 NGFX_WaitForArtifactFilePath_Params;
#define NGFX_WaitForArtifactFilePath_Params_V1_VER NGFX_MAKE_STRUCT_VERSION(NGFX_WaitForArtifactFilePath_Params_V1, 1)
#define NGFX_WaitForArtifactFilePath_Params_VER NGFX_WaitForArtifactFilePath_Params_V1_VER

/**
 * @brief Enum representing the distribution/SKU of Nsight Graphics.
 *
 * The sku indicates the distribution of Nsight Graphics (Public, Pro, Internal, etc.).
 */
typedef enum NGFX_Injection_SKU
{
    NGFX_Injection_SKU_UNKNOWN,  /**< Unknown SKU type. */
    NGFX_Injection_SKU_PUBLIC,   /**< Public distribution of Nsight Graphics. */
    NGFX_Injection_SKU_PRO,      /**< Professional distribution of Nsight Graphics. */
    NGFX_Injection_SKU_INTERNAL, /**< Internal distribution of Nsight Graphics. */
} NGFX_Injection_SKU;

/**
 * @brief Structure containing information about an Nsight Graphics installation.
 */
typedef struct NGFX_InstallationInfo
{
    NGFX_Injection_SKU sku;          /**< The distribution of Nsight in this installation. */
    uint16_t versionMajor;           /**< The major version of the Nsight installation. */
    uint16_t versionMinor;           /**< The minor version of the Nsight installation. */
    uint16_t versionPatch;           /**< The patch version of the Nsight installation. */
    NGFX_PathChar* installationPath; /**< The path on the system to the Nsight installation. */
} NGFX_InstallationInfo;

/**
 * @brief Enum representing the workload boundary reported by Streamline for frame generation
 */
#ifdef __cplusplus
enum NGFX_For_Dlss_DLSS_FG_PresentBoundaryType : int
#else
typedef enum NGFX_For_Dlss_DLSS_FG_PresentBoundaryType
#endif
{
    NGFX_For_Dlss_DLSS_FG_PresentBoundaryType_Application_Present_Requested = 0, /**< Streamline should report application present begin boundary with requested api parameters (even though this call may be completely reinterpreted for FG) **/
    NGFX_For_Dlss_DLSS_FG_PresentBoundaryType_Generated_Frame_Begin = 1,         /**< Streamline is generating and presenting a new frame and should report a generated frame id as well as the queue. **/
    NGFX_For_Dlss_DLSS_FG_PresentBoundaryType_Generated_Frame_End = 2,           /**< Streamline is done with the generation and present of the new frame. **/
    NGFX_For_Dlss_DLSS_FG_PresentBoundaryType_Real_Frame_Begin = 3,              /**< Streamline should issue this before any calls to present the "real frame" to the swapchain. Note this present happens after any generated frame presents. **/
    NGFX_For_Dlss_DLSS_FG_PresentBoundaryType_Real_Frame_End = 4,                /**< Streamline should issue this after any calls to present the "real frame" to the swapchain. It's assumed this resumes normal rendering by the app. **/
}
#ifdef __cplusplus
;
#else
NGFX_For_Dlss_DLSS_FG_PresentBoundaryType;
#endif

/**
 * END addtogroup NGFX_API_CORE
 * @}
 */

#ifdef __cplusplus
} // extern "C"
#endif

#endif // NGFX_TYPES_H

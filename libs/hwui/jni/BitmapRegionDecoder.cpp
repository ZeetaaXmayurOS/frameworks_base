/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#undef LOG_TAG
#define LOG_TAG "BitmapRegionDecoder"

#include "BitmapFactory.h"
#include "CreateJavaOutputStreamAdaptor.h"
#include "GraphicsJNI.h"
#include "Utils.h"

#include "BitmapRegionDecoder.h"
#include "SkBitmap.h"
#include "SkCodec.h"
#include "SkData.h"
#include "SkStream.h"

#include <HardwareBitmapUploader.h>
#include <androidfw/Asset.h>
#include <sys/stat.h>

#include <memory>

using namespace android;

static jobject createBitmapRegionDecoder(JNIEnv* env, sk_sp<SkData> data) {
    auto brd = skia::BitmapRegionDecoder::Make(std::move(data));
    if (!brd) {
        doThrowIOE(env, "Image format not supported");
        return nullObjectReturn("CreateBitmapRegionDecoder returned null");
    }

    return GraphicsJNI::createBitmapRegionDecoder(env, brd.release());
}

static jobject nativeNewInstanceFromByteArray(JNIEnv* env, jobject, jbyteArray byteArray,
                                              jint offset, jint length) {
    AutoJavaByteArray ar(env, byteArray);
    return createBitmapRegionDecoder(env, SkData::MakeWithCopy(ar.ptr() + offset, length));
}

static jobject nativeNewInstanceFromFileDescriptor(JNIEnv* env, jobject clazz,
                                                   jobject fileDescriptor) {
    NPE_CHECK_RETURN_ZERO(env, fileDescriptor);

    jint descriptor = jniGetFDFromFileDescriptor(env, fileDescriptor);

    struct stat fdStat;
    if (fstat(descriptor, &fdStat) == -1) {
        doThrowIOE(env, "broken file descriptor");
        return nullObjectReturn("fstat return -1");
    }

    return createBitmapRegionDecoder(env, SkData::MakeFromFD(descriptor));
}

static jobject nativeNewInstanceFromStream(JNIEnv* env, jobject clazz, jobject is, // InputStream
                                           jbyteArray storage) { // byte[]
    jobject brd = nullptr;
    sk_sp<SkData> data = CopyJavaInputStream(env, is, storage);

    if (data) {
        brd = createBitmapRegionDecoder(env, std::move(data));
    }
    return brd;
}

static jobject nativeNewInstanceFromAsset(JNIEnv* env, jobject clazz, jlong native_asset) {
    Asset* asset = reinterpret_cast<Asset*>(native_asset);
    sk_sp<SkData> data = CopyAssetToData(asset);
    if (!data) {
        return nullptr;
    }

    return createBitmapRegionDecoder(env, data);
}

/*
 * nine patch not supported
 * purgeable not supported
 * reportSizeToVM not supported
 */
static jobject nativeDecodeRegion(JNIEnv* env, jobject, jlong brdHandle, jint inputX,
        jint inputY, jint inputWidth, jint inputHeight, jobject options, jlong inBitmapHandle,
        jlong colorSpaceHandle) {

    // Set default options.
    int sampleSize = 1;
    SkColorType colorType = kN32_SkColorType;
    bool requireUnpremul = false;
    jobject javaBitmap = nullptr;
    bool isHardware = false;
    sk_sp<SkColorSpace> colorSpace = GraphicsJNI::getNativeColorSpace(colorSpaceHandle);
    // Update the default options with any options supplied by the client.
    if (NULL != options) {
        sampleSize = env->GetIntField(options, gOptions_sampleSizeFieldID);
        jobject jconfig = env->GetObjectField(options, gOptions_configFieldID);
        colorType = GraphicsJNI::getNativeBitmapColorType(env, jconfig);
        isHardware = GraphicsJNI::isHardwareConfig(env, jconfig);
        requireUnpremul = !env->GetBooleanField(options, gOptions_premultipliedFieldID);
        javaBitmap = env->GetObjectField(options, gOptions_bitmapFieldID);
        // The Java options of ditherMode and preferQualityOverSpeed are deprecated.  We will
        // ignore the values of these fields.

        // Initialize these fields to indicate a failure.  If the decode succeeds, we
        // will update them later on.
        env->SetIntField(options, gOptions_widthFieldID, -1);
        env->SetIntField(options, gOptions_heightFieldID, -1);
        env->SetObjectField(options, gOptions_mimeFieldID, 0);
        env->SetObjectField(options, gOptions_outConfigFieldID, 0);
        env->SetObjectField(options, gOptions_outColorSpaceFieldID, 0);
    }

    // Recycle a bitmap if possible.
    android::Bitmap* recycledBitmap = nullptr;
    size_t recycledBytes = 0;
    if (javaBitmap) {
        recycledBitmap = &bitmap::toBitmap(inBitmapHandle);
        if (recycledBitmap->isImmutable()) {
            ALOGW("Warning: Reusing an immutable bitmap as an image decoder target.");
        }
        recycledBytes = recycledBitmap->getAllocationByteCount();
    }

    auto* brd = reinterpret_cast<skia::BitmapRegionDecoder*>(brdHandle);
    SkColorType decodeColorType = brd->computeOutputColorType(colorType);

    if (isHardware) {
        if (decodeColorType == kRGBA_F16_SkColorType &&
            !uirenderer::HardwareBitmapUploader::hasFP16Support()) {
            decodeColorType = kN32_SkColorType;
        }
        if (decodeColorType == kRGBA_1010102_SkColorType &&
            !uirenderer::HardwareBitmapUploader::has1010102Support()) {
            decodeColorType = kN32_SkColorType;
        }
    }

    // Set up the pixel allocator
    skia::BRDAllocator* allocator = nullptr;
    RecyclingClippingPixelAllocator recycleAlloc(recycledBitmap, recycledBytes);
    HeapAllocator heapAlloc;
    if (javaBitmap) {
        allocator = &recycleAlloc;
        // We are required to match the color type of the recycled bitmap.
        decodeColorType = recycledBitmap->info().colorType();
    } else {
        allocator = &heapAlloc;
    }

    sk_sp<SkColorSpace> decodeColorSpace = brd->computeOutputColorSpace(
            decodeColorType, colorSpace);

    // Decode the region.
    SkIRect subset = SkIRect::MakeXYWH(inputX, inputY, inputWidth, inputHeight);
    SkBitmap bitmap;
    if (!brd->decodeRegion(&bitmap, allocator, subset, sampleSize,
            decodeColorType, requireUnpremul, decodeColorSpace)) {
        return nullObjectReturn("Failed to decode region.");
    }

    // If the client provided options, indicate that the decode was successful.
    if (NULL != options) {
        env->SetIntField(options, gOptions_widthFieldID, bitmap.width());
        env->SetIntField(options, gOptions_heightFieldID, bitmap.height());

        env->SetObjectField(options, gOptions_mimeFieldID,
                getMimeTypeAsJavaString(env, brd->getEncodedFormat()));
        if (env->ExceptionCheck()) {
            return nullObjectReturn("OOM in encodedFormatToString()");
        }

        jint configID = GraphicsJNI::colorTypeToLegacyBitmapConfig(decodeColorType);
        if (isHardware) {
            configID = GraphicsJNI::kHardware_LegacyBitmapConfig;
        }
        jobject config = env->CallStaticObjectMethod(gBitmapConfig_class,
                gBitmapConfig_nativeToConfigMethodID, configID);
        env->SetObjectField(options, gOptions_outConfigFieldID, config);

        env->SetObjectField(options, gOptions_outColorSpaceFieldID,
                GraphicsJNI::getColorSpace(env, decodeColorSpace.get(), decodeColorType));
    }

    // If we may have reused a bitmap, we need to indicate that the pixels have changed.
    if (javaBitmap) {
        recycleAlloc.copyIfNecessary();
        bitmap::reinitBitmap(env, javaBitmap, recycledBitmap->info(), !requireUnpremul);
        return javaBitmap;
    }

    int bitmapCreateFlags = 0;
    if (!requireUnpremul) {
        bitmapCreateFlags |= android::bitmap::kBitmapCreateFlag_Premultiplied;
    }
    if (isHardware) {
        sk_sp<Bitmap> hardwareBitmap = Bitmap::allocateHardwareBitmap(bitmap);
        return bitmap::createBitmap(env, hardwareBitmap.release(), bitmapCreateFlags);
    }
    return android::bitmap::createBitmap(env, heapAlloc.getStorageObjAndReset(), bitmapCreateFlags);
}

static jint nativeGetHeight(JNIEnv* env, jobject, jlong brdHandle) {
    auto* brd = reinterpret_cast<skia::BitmapRegionDecoder*>(brdHandle);
    return static_cast<jint>(brd->height());
}

static jint nativeGetWidth(JNIEnv* env, jobject, jlong brdHandle) {
    auto* brd = reinterpret_cast<skia::BitmapRegionDecoder*>(brdHandle);
    return static_cast<jint>(brd->width());
}

static void nativeClean(JNIEnv* env, jobject, jlong brdHandle) {
    auto* brd = reinterpret_cast<skia::BitmapRegionDecoder*>(brdHandle);
    delete brd;
}

///////////////////////////////////////////////////////////////////////////////

static const JNINativeMethod gBitmapRegionDecoderMethods[] = {
    {   "nativeDecodeRegion",
        "(JIIIILandroid/graphics/BitmapFactory$Options;JJ)Landroid/graphics/Bitmap;",
        (void*)nativeDecodeRegion},

    {   "nativeGetHeight", "(J)I", (void*)nativeGetHeight},

    {   "nativeGetWidth", "(J)I", (void*)nativeGetWidth},

    {   "nativeClean", "(J)V", (void*)nativeClean},

    {   "nativeNewInstance",
        "([BII)Landroid/graphics/BitmapRegionDecoder;",
        (void*)nativeNewInstanceFromByteArray
    },

    {   "nativeNewInstance",
        "(Ljava/io/InputStream;[B)Landroid/graphics/BitmapRegionDecoder;",
        (void*)nativeNewInstanceFromStream
    },

    {   "nativeNewInstance",
        "(Ljava/io/FileDescriptor;)Landroid/graphics/BitmapRegionDecoder;",
        (void*)nativeNewInstanceFromFileDescriptor
    },

    {   "nativeNewInstance",
        "(J)Landroid/graphics/BitmapRegionDecoder;",
        (void*)nativeNewInstanceFromAsset
    },
};

int register_android_graphics_BitmapRegionDecoder(JNIEnv* env)
{
    return android::RegisterMethodsOrDie(env, "android/graphics/BitmapRegionDecoder",
            gBitmapRegionDecoderMethods, NELEM(gBitmapRegionDecoderMethods));
}

/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_SandboxPolicyHWInference_h
#define mozilla_SandboxPolicyHWInference_h

namespace mozilla {

static const char SandboxPolicyHWInference[] = R"SANDBOX_LITERAL(
  (version 1)

  (define shouldLog (param "SHOULD_LOG"))
  (define appPath (param "APP_PATH"))
  (define userCacheDir (param "DARWIN_USER_CACHE_DIR"))
  (define bundleIDCacheDir (param "BUNDLE_ID_CACHE_DIR"))
  (define crashPort (param "CRASH_PORT"))
  (define macosVersion (string->number (param "MAC_OS_VERSION")))
  (define isRosettaTranslated (param "IS_ROSETTA_TRANSLATED"))

  (define (moz-deny feature)
    (if (string=? shouldLog "TRUE")
      (deny feature)
      (deny feature (with no-log))))

  (moz-deny default)
  (moz-deny process-info*)
  (moz-deny nvram*)
  (moz-deny iokit-get-properties)
  (moz-deny file-map-executable)

  (allow process-info-pidinfo process-info-setcontrol (target self))
  (allow file-read-metadata (subpath "/"))
  (allow file-map-executable file-read*
    (subpath "/System")
    (subpath "/usr/lib")
    (subpath "/Library/GPUBundles")
    (subpath appPath))

  (allow signal (target self))
  (allow file-read*
    (literal "/dev/random")
    (literal "/dev/urandom"))

  (if (string? crashPort)
    (allow mach-lookup (global-name crashPort)))

  ; Basic sysctls for CPU info (needed for optimization decisions)
  (allow sysctl-read
    (sysctl-name "hw.memsize")
    (sysctl-name "hw.ncpu")
    (sysctl-name "hw.activecpu")
    (sysctl-name "hw.logicalcpu_max")
    (sysctl-name "hw.physicalcpu_max")
    (sysctl-name "hw.perflevel0.logicalcpu")
    (sysctl-name "hw.perflevel0.physicalcpu")
    (sysctl-name "hw.perflevel1.logicalcpu")
    (sysctl-name "hw.perflevel1.physicalcpu"))

  ; Metal shader compiler service
  (allow mach-lookup
    (global-name "com.apple.MTLCompilerService"))

  ; Shader cache directory access
  (allow file-read* file-write*
    (require-all
      (require-not (vnode-type SYMLINK))
      (subpath bundleIDCacheDir)))

  ; Allow issuing sandbox extensions for the MTLCompilerService process
  ; to access shader cache directories. Only needed on macOS 14 and earlier.
  (if (<= macosVersion 1500)
    (allow file-issue-extension
      (require-all
        (extension-class "com.apple.app-sandbox.read-write")
        (require-not (vnode-type SYMLINK))
        (require-any
          (subpath (string-append bundleIDCacheDir "/com.apple.metalfe"))
          (subpath (string-append bundleIDCacheDir "/com.apple.gpuarchiver"))))))

  ; Minimal IOKit properties for Metal device identification
  (allow iokit-get-properties
    (iokit-property "vendor-id")
    (iokit-property "device-id")
    (iokit-property "IOVARendererID")
    (iokit-property "MetalPluginName")
    (iokit-property "MetalPluginClassName")
    (iokit-property "gpu-core-count"))

  ; IOKit connections required for Metal compute
  (allow iokit-open
    (iokit-connection "IOAccelerator")
    (iokit-user-client-class "IOAccelerationUserClient")
    (iokit-user-client-class "AGPMClient")
    (iokit-user-client-class "AppleGraphicsControlClient")
    (iokit-user-client-class "AppleMGPUPowerControlClient")
    (iokit-user-client-class "AppleGraphicsPolicyClient"))

  (if (string=? isRosettaTranslated "TRUE")
    (allow file-map-executable (subpath "/private/var/db/oah")))
)SANDBOX_LITERAL";

}  // namespace mozilla

#endif  // mozilla_SandboxPolicyHWInference_h
# Payback 2 Port — Shim Manifest

> [!NOTE]
> Project direction has transitioned to a **Web Port (WebAssembly + WebGPU/WebGL2)**.
> This manifest defines the exact symbol surface that the WebAssembly runtime environment must supply.
> In the Web target:
> - `libc`/`libm` map to Emscripten libc/musl.
> - `EGL`/`GLES2` map to WebGPU (or WebGL2).
> - `OpenSL` maps to Web Audio API.
> - Android NDK APIs map to the Web/DOM VFS and event loop.

The complete symbol surface the port must provide, merged from both truth sources:

1. **reloc** — undefined symbols in `.rela.plt`/`.rela.dyn` of the patched `.so`
   (Android NDK APIs, EGL/GL, math, a few libc, logging, OpenSL).
2. **oracle** — PairIP **stripped the relocation entries** for ~120 `.got.plt` slots;
   their true libc functions were recovered by symbolizing the *live* process GOT
   (`runtime_dump/got_truth.json`). Every stripped slot is listed here.

Strategy key:

- **map** — point the GOT slot straight at the macOS libc/libm symbol (via `dlsym`).
  Correct because AAPCS64 and Apple arm64 agree for non-variadic, non-struct-ABI calls.
- **shim** — hand-written boundary (Bionic layout/flag/varargs differences).
- **stub** — accepted and faked (PairIP VM, OpenSL); offline play must not need it.
- **real** — the actual port functionality (window/GL/assets/sensors); P1 stubs count as shim.

Totals: **268 symbols** — map: 98, real: 106, shim: 62, stub: 2

## real (106)

```
AAssetManager_fromJava                       reloc          
AAssetManager_open                           reloc          
AAsset_close                                 reloc          
AAsset_getLength                             reloc          
AAsset_getRemainingLength                    reloc          
AAsset_read                                  reloc          
AConfiguration_delete                        reloc          
AConfiguration_fromAssetManager              reloc          
AConfiguration_getCountry                    reloc          
AConfiguration_getLanguage                   reloc          
AConfiguration_new                           reloc          
ALooper_acquire                              reloc          
ALooper_addFd                                reloc          
ALooper_forThread                            reloc          
ALooper_pollOnce                             reloc          
ALooper_prepare                              reloc          
ALooper_release                              reloc          
ALooper_removeFd                             reloc          
ANativeWindow_acquire                        reloc          
ANativeWindow_fromSurface                    reloc          
ANativeWindow_getHeight                      reloc          
ANativeWindow_getWidth                       reloc          
ANativeWindow_release                        reloc          
ANativeWindow_setBuffersGeometry             reloc          
ASensorEventQueue_disableSensor              reloc          
ASensorEventQueue_enableSensor               reloc          
ASensorEventQueue_getEvents                  reloc          
ASensorEventQueue_setEventRate               reloc          
ASensorManager_createEventQueue              reloc          
ASensorManager_getDefaultSensor              reloc          
ASensorManager_getInstance                   reloc          
SL_IID_ANDROIDSIMPLEBUFFERQUEUE              reloc          
SL_IID_ENGINE                                reloc          
SL_IID_PLAY                                  reloc          
__system_property_get                        reloc          
eglChooseConfig                              reloc          
eglCreateContext                             reloc          
eglCreateWindowSurface                       reloc          
eglDestroyContext                            reloc          
eglDestroySurface                            reloc          
eglGetConfigAttrib                           reloc          
eglGetDisplay                                reloc          
eglInitialize                                reloc          
eglMakeCurrent                               reloc          
eglQuerySurface                              reloc          
eglSwapBuffers                               reloc          
eglTerminate                                 reloc          
glActiveTexture                              reloc          
glAttachShader                               reloc          
glBindBuffer                                 reloc          
glBindFramebuffer                            reloc          
glBindRenderbuffer                           reloc          
glBindTexture                                reloc          
glBlendFunc                                  reloc          
glBufferData                                 reloc          
glBufferSubData                              reloc          
glCheckFramebufferStatus                     reloc          
glClear                                      reloc          
glClearColor                                 reloc          
glCompileShader                              reloc          
glCompressedTexImage2D                       reloc          
glCreateProgram                              reloc          
glCreateShader                               reloc          
glDeleteBuffers                              reloc          
glDeleteTextures                             reloc          
glDepthMask                                  reloc          
glDisable                                    reloc          
glDisableVertexAttribArray                   reloc          
glDrawArrays                                 reloc          
glDrawElements                               reloc          
glEnable                                     reloc          
glEnableVertexAttribArray                    reloc          
glFlush                                      reloc          
glFramebufferRenderbuffer                    reloc          
glFramebufferTexture2D                       reloc          
glGenBuffers                                 reloc          
glGenFramebuffers                            reloc          
glGenRenderbuffers                           reloc          
glGenTextures                                reloc          
glGenerateMipmap                             reloc          
glGetAttribLocation                          reloc          
glGetIntegerv                                reloc          
glGetProgramInfoLog                          reloc          
glGetProgramiv                               reloc          
glGetShaderInfoLog                           reloc          
glGetShaderiv                                reloc          
glGetString                                  reloc          
glGetUniformLocation                         reloc          
glLinkProgram                                reloc          
glReadPixels                                 reloc          
glRenderbufferStorage                        reloc          
glShaderSource                               reloc          
glTexImage2D                                 reloc          
glTexParameterf                              reloc          
glTexParameteri                              reloc          
glTexSubImage2D                              reloc          
glUniform1f                                  reloc          
glUniform1i                                  reloc          
glUniform2f                                  reloc          
glUniform3f                                  reloc          
glUniform4f                                  reloc          
glUniform4fv                                 reloc          
glUniformMatrix4fv                           reloc          
glUseProgram                                 reloc          
glVertexAttribPointer                        reloc          
glViewport                                   reloc          
```

## shim (62)

```
__android_log_assert                         reloc          
__android_log_print                          reloc          
__cxa_thread_atexit_impl                     reloc          
__errno                                      reloc          
__memcmp_aarch64                             oracle         libc.so
__memcpy_aarch64_simd                        oracle         libc.so
__memset_aarch64                             oracle         libc.so
__register_atfork                            reloc          
__sF                                         reloc          
__stack_chk_fail                             reloc          
android_set_abort_message                    oracle         libc.so
connect                                      reloc          
dl_iterate_phdr                              reloc          
fcntl                                        oracle         libc.so
fprintf                                      oracle         libc.so
freeaddrinfo                                 oracle         libc.so
fscanf                                       oracle         libc.so
getaddrinfo                                  oracle         libc.so
getauxval                                    oracle         libc.so
nice                                         oracle         libc.so
pipe                                         oracle         libc.so
pthread_attr_init                            oracle         libc.so
pthread_attr_setdetachstate                  oracle         libc.so
pthread_cond_broadcast                       oracle         libc.so
pthread_cond_destroy                         oracle         libc.so
pthread_cond_init                            oracle         libc.so
pthread_cond_signal                          oracle         libc.so
pthread_cond_timedwait                       oracle         libc.so
pthread_cond_wait                            oracle         libc.so
pthread_create                               oracle         libc.so
pthread_detach                               oracle         libc.so
pthread_getschedparam                        oracle         libc.so
pthread_getspecific                          oracle         libc.so
pthread_join                                 oracle         libc.so
pthread_key_create                           oracle         libc.so
pthread_key_delete                           oracle         libc.so
pthread_mutex_destroy                        oracle         libc.so
pthread_mutex_init                           oracle         libc.so
pthread_mutex_lock                           oracle         libc.so
pthread_mutex_trylock                        oracle         libc.so
pthread_mutex_unlock                         oracle         libc.so
pthread_mutexattr_destroy                    oracle         libc.so
pthread_mutexattr_init                       oracle         libc.so
pthread_mutexattr_settype                    oracle         libc.so
pthread_once                                 oracle         libc.so
pthread_rwlock_rdlock                        oracle         libc.so
pthread_rwlock_unlock                        oracle         libc.so
pthread_rwlock_wrlock                        oracle         libc.so
pthread_self                                 oracle         libc.so
pthread_setspecific                          oracle         libc.so
setsockopt                                   oracle         libc.so
snprintf                                     oracle         libc.so
socket                                       reloc          
sprintf                                      oracle         libc.so
strerror_r                                   oracle         libc.so
swprintf                                     oracle         libc.so
sync                                         oracle         libc.so
syscall                                      oracle         libc.so
sysconf                                      oracle         libc.so
syslog                                       oracle         libc.so
vasprintf                                    oracle         libc.so
vfprintf                                     oracle         libc.so
```

## map (98)

```
__cxa_atexit                                 reloc          
__cxa_finalize                               reloc          
__stack_chk_guard                            reloc          
abort                                        oracle         libc.so
acosf                                        reloc          
arc4random                                   oracle         libc.so
asinf                                        reloc          
atan2                                        reloc          
atan2f                                       reloc          
atanf                                        reloc          
atoi                                         oracle         libc.so
calloc                                       oracle         libc.so
cbrtf                                        reloc          
close                                        reloc          
closelog                                     oracle         libc.so
cos                                          reloc          
cosf                                         reloc          
exit                                         oracle         libc.so
exp                                          reloc          
exp2                                         reloc          
expf                                         reloc          
fclose                                       oracle         libc.so
fflush                                       oracle         libc.so
fmodf                                        reloc          
fopen                                        oracle         libc.so
fputc                                        oracle         libc.so
fread                                        oracle         libc.so
free                                         oracle         libc.so
fseek                                        oracle         libc.so
ftell                                        oracle         libc.so
fwrite                                       oracle         libc.so
gettimeofday                                 oracle         libc.so
ioctl                                        oracle         libc.so
localtime                                    oracle         libc.so
log                                          reloc          
log10                                        reloc          
logf                                         reloc          
malloc                                       oracle         libc.so
memchr                                       reloc          
nanosleep                                    oracle         libc.so
openlog                                      oracle         libc.so
posix_memalign                               oracle         libc.so
pow                                          reloc          
powf                                         reloc          
puts                                         oracle         libc.so
qsort                                        oracle         libc.so
rand                                         oracle         libc.so
read                                         oracle         libc.so
realloc                                      oracle         libc.so
recv                                         oracle         libc.so
remove                                       oracle         libc.so
rename                                       oracle         libc.so
sched_yield                                  oracle         libc.so
select                                       oracle         libc.so
send                                         oracle         libc.so
sincos                                       reloc          
sincosf                                      reloc          
sinf                                         reloc          
sleep                                        oracle         libc.so
srand                                        oracle         libc.so
strcasecmp                                   oracle         libc.so
strcat                                       oracle         libc.so
strchr                                       reloc          
strcmp                                       reloc          
strcpy                                       reloc          
strcspn                                      oracle         libc.so
strdup                                       oracle         libc.so
strerror                                     oracle         libc.so
strftime                                     oracle         libc.so
strlen                                       reloc          
strncmp                                      reloc          
strncpy                                      oracle         libc.so
strpbrk                                      oracle         libc.so
strstr                                       oracle         libc.so
strtod                                       oracle         libc.so
strtof                                       oracle         libc.so
strtok                                       oracle         libc.so
strtol                                       oracle         libc.so
strtold                                      oracle         libc.so
strtoll                                      oracle         libc.so
strtoul                                      oracle         libc.so
strtoull                                     oracle         libc.so
tanf                                         reloc          
time                                         oracle         libc.so
usleep                                       oracle         libc.so
wcscmp                                       oracle         libc.so
wcscpy                                       oracle         libc.so
wcslen                                       oracle         libc.so
wcstod                                       oracle         libc.so
wcstof                                       oracle         libc.so
wcstol                                       oracle         libc.so
wcstold                                      oracle         libc.so
wcstoll                                      oracle         libc.so
wcstoul                                      oracle         libc.so
wcstoull                                     oracle         libc.so
wmemchr                                      oracle         libc.so
wmemcmp                                      oracle         libc.so
write                                        oracle         libc.so
```

## stub (2)

```
ExecuteProgram                               reloc          
slCreateEngine                               reloc          
```

## Boundary ABI landmines (why `shim` is not optional)

1. **Variadic functions.** Apple arm64 passes *all* variadic arguments on the
   stack; AAPCS64 passes them in registers first. A guest `printf(fmt, 1)` lands
   with `1` in `x1`, but a host `printf` compiled by Apple clang reads it from the
   stack. All printf-family/log imports therefore need guest-ABI entry stubs that
   marshal (x1..x7, stack) into a canonical form and run a local formatter.
   The guest's own `va_list` (AAPCS64 struct: `__stack,__gr_top,__vr_top,
   `__gr_offs,__vr_offs`) must be interpreted directly for `vsnprintf` etc.
2. **TLS.** Bionic `TPIDR_EL0` TCB layout: slot 5 (byte 0x28) = stack canary;
   slot 2 = errno. The engine reads `tpidr_el0+0x28` on every guarded function.
   The port must give each guest thread a TCB with a stable canary at +0x28 and
   the guest errno at +0x10, while leaving Apple's own TLS intact for host libc
   (empirical check scheduled: which alias does host libc use, TPIDR_EL0 or
   TPIDRRO_EL0 — determines whether we need save/restore around shim calls).
3. **File-descriptor flags.** Linux `O_CREAT=0x40 O_TRUNC=0x200 O_APPEND=0x400
   O_NONBLOCK=0x800`; macOS `O_CREAT=0x200 O_TRUNC=0x400 O_APPEND=0x8
   O_NONBLOCK=0x4`. `fcntl`/`open`/`pipe` shims translate both directions.
4. **Socket structs.** BSD `sockaddr` carries `sa_len` at offset 0 and `sa_family`
   at offset 1; Linux has a 16-bit family at offset 0. `connect`/`getaddrinfo`
   results need translation. `AF_INET6` 10 vs 30, `SOCK_NONBLOCK` in-type on
   Linux vs fcntl on macOS.
5. **pthread objects.** Bionic mutex 40 B / cond 48 B / rwlock 56 B / attr 56 B;
   Darwin mutex 64 B (with signature), cond 48 B but different opcode semantics.
   Shim keeps a side table guest-address → host primitive. Bionic cond vars
   default to `CLOCK_MONOTONIC`; Darwin `pthread_cond_timedwait` is `CLOCK_REALTIME`
   → convert absolute deadlines. `PTHREAD_MUTEX_RECURSIVE` is 1 on Bionic, 2 on
   Darwin.
6. **stdio identity.** `__sF[3]` (Bionic's stdin/stdout/stderr array) is where
   `&__sF[2]` comes from; FILE* values cross the boundary, so `fopen`-family shims
   return guest-side tokens mapped to host FILEs.
7. **`strerror_r`**: Android = GNU variant (returns `char*`), macOS = XSI
   (returns `int`). `sysconf`/`getauxval` constants differ (e.g. `_SC_NPROCESSORS_ONLN`).

# Static Analysis Report — Payback 2: The Battle Sandbox

**Artifact analysed:** `net.apex_designs.payback2_2.106.16-178_2arch_7dpi_24lang_...apkm`
**Package:** `net.apex_designs.payback2` · **Version:** 2.106.16 (versionCode 178)
**Publisher:** Apex Designs Entertainment Ltd (CN=James Daniels)
**Build date:** 2025-08-28 · **Analysis date:** 2026-10-15
**Method:** static analysis only (apktool 3.0.3, jadx, custom DEX/ELF parsers). No runtime execution, no traffic capture.

> Note on scope: the file is an **`.apkm` bundle**, not a single APK. It contains `base.apk` plus 33 configuration splits (2 ABI, 24 language, 7 density). All findings below refer to `base.apk` (the code + assets) plus the ABI splits (native libraries).

---

## 1. Executive Summary

Payback 2 is a legitimate, long-lived commercial Android game — a 3D open-world sandbox brawler with integrated advertising and in-app purchases. It is **not** a malicious or repackaged application: the signature matches the original developer key preserved from the App Bundle, and no obfuscated dropper, packer, or dynamic-code-loading behaviour was found.

However, the analysis surfaced **several material concerns**:

| Area | Rating | Summary |
|---|---|---|
| Malware / backdoors | **None found** | No C2, no dynamic code loading, no hidden payloads, no root exploits |
| Signing & authenticity | **Authentic** | Original dev key; extra runtime signature pinning via PAIRIP |
| Privacy / consent | **Concerning** | ByteDance/Pangle ad consent is **hardcoded to "granted"**, bypassing user choice |
| Transport security | **Weak** | `usesCleartextTraffic="true"`; privacy policy, FAQ and invite links served over **plain HTTP** |
| Credentials | **Poor hygiene** | Live Twitter + Facebook OAuth consumer secrets committed in `assets/` |
| IAP integrity | **Weak** | Purchases granted **client-side only** — no signature check, no server-side receipt validation |
| Code quality | **Mixed** | Multiple dead code paths, stubbed features, an unhandled-crash bug, and a broken notification feature on modern Android |
| Packaging efficiency | **Poor** | 77.7 MB of native libraries, of which ~69 MB is zero padding |
| Dependency hygiene | **Very heavy** | 8 mediation layers and ~14 top-level namespaces; multiple superseded/deprecated ad SDKs |

The app is best characterised as a **well-functioning but technically ageing indie game**: competent at its core, but carrying a decade of accumulated ad-SDK and platform debt, with a handful of genuine privacy and security defects.

---

## 2. Identity, Packaging & Build Provenance

| Property | Value |
|---|---|
| Package name | `net.apex_designs.payback2` |
| versionName / versionCode | `2.106.16` / `178` |
| minSdkVersion | 23 (Android 6.0) |
| targetSdkVersion | 35 (Android 15) |
| compileSdkVersion | 35 |
| Application class | `com.pairip.application.Application` |
| Android Gradle Plugin | 8.11.1 (R8/D8/L8 8.11.18, full mode) |
| Native toolchain | NDK `Android (13818152, +pgo, +bolt, +lto, +mlgo)` |
| Multidex | Yes — 8 DEX files, 37.5 MB |
| App category | `game` |
| GL requirement | OpenGL ES 2.0 (`glEsVersion=0x20000`, required) + `ETC1_RGB8` texture support |
| Orientation | `sensorLandscape` (locked to landscape) |
| Splits | 2 ABIs (arm64-v8a, armeabi-v7a), 24 languages, 7 densities (120–640 dpi) |
| `com.android.vending.splits.required` | `true` (must be installed via a split-aware installer) |

### 2.1 Signing

```
Owner/Issuer : CN=James Daniels, O=Apex Designs Entertainment Ltd
Serial       : 10e95a7b
Valid        : 2014-08-12 → 2064-07-30  (50-year developer key)
Algorithm    : SHA256withRSA, 2048-bit key, X.509 v3
SHA-1        : EF:A0:68:13:55:68:E9:CF:8D:05:B4:B7:15:72:25:88:EC:86:F9:A4
SHA-256      : EC:E9:21:7D:19:3B:F0:B2:5C:82:C8:EF:89:8A:66:64:50:38:44:41:8A:61:1B:0C:A0:02:B0:1C:E5:E1:36:49
Signature file: META-INF/BNDLTOOL.RSA (v1/JAR scheme retained from AAB export)
```

The signature block is `BNDLTOOL.*`, meaning the APK was generated from an Android App Bundle and re-signed with the **original upload key** — APKMirror did not re-sign it. Combined with the presence of a Play Store distribution stamp (`stamp-cert-sha256`, `STAMP_TYPE_DISTRIBUTION_APK`), this is a **genuine, unmodified build**.

### 2.2 Anti-tampering

The app additionally runs its own signing-certificate check at startup (`com/pairip/SignatureCheck.java`):

```java
private static final String ALLOWLISTED_SIG = "Vn3kj4pUblROi2S+QfRRL9nhsaO2uoHQg6+dpEtxdTE=";
private static String expectedSignature = "7OkhfRk78LJcgsjviYpmZFA4REGKYRsMoAKwHOXhNkk=";
// verifyIntegrity(): SHA-256 of PackageInfo.signatures[0], base64 → compared
// mismatch → throw new SignatureTamperedException("Apk signature is invalid.")
```

This is **PAIRIP** (Google's license/tamper protection for Play-distributed apps). It blocks repackaging and patched-APK redistribution. Two weaknesses are worth noting:

- `signatures[0]` on API 28+ returns only the current signer, not the full signing-certificate *history*, so rotation scenarios are not handled robustly.
- `ALLOWLISTED_SIG` is a second accepted key — almost certainly the Play **upload/debug** key. Any build signed with that key bypasses the check. It should be narrowed or removed from release builds.

---

## 3. Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  PaybackActivity  (com.google.androidgamesdk.GameActivity)      │
│  ├── native bridge:  11 × GAME_*,  ~40 × ANDROID_*/HW_*/GMS_*   │
│  ├── AdMob (Google Mobile Ads 24.5.0)  — direct                 │
│  ├── IronSource LevelPlay 8.9.1         — direct                 │
│  ├── Google Play Billing 7.0.0, Play Games 23.1.0, Sign-In 20.7.0│
│  └── WebViewActivity / VideoPlayerActivity / ReminderService     │
└───────────────────────────┬──────────────────────────────────────┘
                            │  JNI
┌───────────────────────────▼──────────────────────────────────────┐
│  libpayback2.so  — custom C++ OpenGL ES 2.0 engine (2.7 MB .text)│
│  ├── AGAPI networking  → %s/<cmd>.php  backend (apex-designs.net) │
│  ├── ExitGames PHOTON  → realtime multiplayer (LoadBalancing,Chat)│
│  ├── CRN texture decoder (crn/crn_decomp.h)                     │
│  ├── fetchServerTimestamp → server-time anti-cheat               │
│  └── 34.6 MB .bss arena → runtime asset mapping/decryption      │
└──────────────────────────────────────────────────────────────────┘
```

### 3.1 First-party code surface

The app owns only **five Java classes** (`jadx_out/sources/net/apex_designs/payback2/`):

| Class | Role |
|---|---|
| `PaybackActivity` (2,723 lines) | Everything: launcher, ad control, IAP, Play Games, consent, audio, deep links |
| `WebViewActivity` | Facebook share sheet in a WebView |
| `VideoPlayerActivity` | Intro/trailer playback |
| `NotificationQueue` | File-backed queue + `AlarmManager` scheduling for reminder notifications |
| `ReminderService` | `BroadcastReceiver` that fires the reminder notification |

Everything else is a bundled SDK. **~95% of the 20,562 decompiled classes belong to third-party code.** The real product logic lives in the native library.

### 3.2 Engine

This is a **custom in-house engine**, not Unity, Unreal, Cocos2d-x or libGDX. Evidence:

- Plain EGL calls (`eglChooseConfig`, `eglGetConfigAttrib`) and NDK `AConfiguration_*` — no engine framework symbols.
- Proprietary binary content formats: `.amct` (compressed texture atlas, CRN-derived), `.pak` (map/environment packs), `.dat` (meshes, animation, sound, phrase tables), `.glsl` (76 shaders), `.ama` (speech clips, 514 files).
- Asset tree referenced directly from native strings:
  ```
  Data/Maps/%d.env.pak
  Data/Maps/MapTextures/{Opaque_Diff,Opaque_SpecNorm,Alpha_Diff,...}_{0..3}[_hq].amct
  Data/Additional/Obj{Tri,Vert,HullEdge,HullTri,Skinning}*.dat
  Data/Additional/Phrases_{data,level,main}.dat     ← localisation tables
  Data/Additional/SoundData.dat
  Data/Speech/*.ama
  Data/Shaders/*.glsl
  ```
- A CRN decompressor is compiled in (`crnd_free: bad ptr`, `crnd_malloc: out of memory`), sourced from `src/main/jni/crn/crn_decomp.h`.

**Localisation is data-driven:** the 24 language splits only carry `resources.arsc` (SDK/UI strings); the game's own text lives in `Phrases_*.dat` and is surfaced to Java via `ANDROID_ResetPhrases` / `ANDROID_StorePhrase` / `GetPhrase(int)`.

**Multiplayer** is built on the **ExitGames Photon** SDK (statically linked into the native lib): `LoadBalancing::Client`, `Photon::PhotonPeer`, `Chat::Client`, `EnetConnect`, plus Photon's own `Encryption::{encrypt,decrypt,_xor,calculateSHA256Hash}` and `opExchangeKeysForEncryption`. Deep link `payback2://invite=` carries room invites.

### 3.3 Leaked developer build path

The native library retains an absolute build-time path:

```
/Users/james/Documents/AndroidStudio/Payback2/payback2/src/main/jni/crn/crn_decomp.h
```

This discloses the developer's macOS username (`james`), the IDE, the Gradle module layout, and the vendored CRN source path. Minor, but it is an information leak that should be stripped via `-ffile-prefix-map`.

---

## 4. Size & Packaging Analysis

### 4.1 base.apk (157.1 MB uncompressed)

| Component | Files | Size | % |
|---|---:|---:|---:|
| `assets/Data/Maps` (textures, environment packs) | 109 | 64.13 MB | 40.8% |
| `assets/Data/Additional` (textures, meshes, phrases) | 110 | 42.15 MB | 26.8% |
| DEX (Java/Kotlin glue + all SDKs) | 8 | 37.48 MB | 23.9% |
| `res/` | 397 | 6.59 MB | 4.2% |
| `assets/` encrypted packs + misc | 32 | 3.13 MB | 2.0% |
| `assets/Data/Speech` | 514 | 2.67 MB | 1.7% |
| `META-INF/` (57 `.version` markers) | 57 | 0.30 MB | 0.2% |
| `assets/Data/Shaders` | 76 | 0.23 MB | 0.1% |
| `resources.arsc` | 1 | 0.20 MB | 0.1% |

Compression ratio **0.994** — 897 of 1,363 entries are stored uncompressed (deliberate, so `mmap`-able assets and DEX are never inflated at install time). All `.amct`, `.pak`, `.dat`, `.ama` texture/pack payloads are **stored, not deflated**, because they are already compressed.

### 4.2 Native libraries — 77.7 MB across two ABIs

| Library | arm64-v8a | armeabi-v7a | Owner |
|---|---:|---:|---|
| `libpayback2.so` | 38.57 MB | 36.19 MB | Apex Designs (game engine) |
| `libpairipcore.so` | 525 KB | 484 KB | PAIRIP DRM |
| `libtt_ugen_layout.so` | 510 KB | 374 KB | ByteDance/Pangle |
| `libnms.so` | 298 KB | 198 KB | ByteDance/Pangle |
| `libapminsightb.so` | 124 KB | 100 KB | APMInsight (ByteDance APM) |
| `libapminsighta.so` | 102 KB | 86 KB | APMInsight |
| `libpglarmor.so` | 69 KB | 47 KB | ByteDance/Pangle (packer) |
| others (4 libs) | 25 KB | 17 KB | ByteDance/Pangle |

**Anatomy of `libpayback2.so` (arm64):**

| Section | Size | Entropy | Meaning |
|---|---:|---:|---|
| `.text` | 2.70 MB | 7.22 | Executable code |
| `.rodata` | 0.27 MB | 5.28 | Constants, Photon tables, strings |
| `.bss` | **34.61 MB** | 0.037 | Runtime asset arena (zero-filled) |

The library file is 38.57 MB but contains only ~3.6 MB of code and data; the remaining ~34.65 MB is **zero padding** backing the huge `.bss` reservation used as a scratch arena for streaming/decrypting assets at runtime.

**Consequence:** the app installs ~78 MB of native libraries — of which ~69 MB (89%) is empty space — to ship 2.7 MB of engine code. This is a **packaging defect**: it inflates installed size and Play Store size reporting substantially. (Download impact is minimal because zeros compress ~1000:1, which is why the ABI splits are only 2.4 MB over the wire.)

### 4.3 Asset container formats

**27 assets** carry meaningless 16-character names and share the header magic `\x00IAP` (3.01 MB total, ~100 KB each — consistent with one pack per language):

```
00000000: 0049 4150 0200 0000 0806 7c93 8bda 3f73  .IAP......|...?s
```

Shannon entropy of the first 100 KB of every sampled blob is **7.91–7.96 bits/byte** (8.0 = indistinguishable from random).

**Correction from first-pass analysis:** the maximum entropy across the asset tree turned out to be **compression, not encryption**. Further testing established the actual formats:

| Format | Count | Size | Verdict |
|---|---:|---:|---|
| `.pak` + `.dat` | 89 | 26.6 MB | **Raw LZMA (`FORMAT_ALONE`), not encrypted.** Header = `props(1) + dict_size(4 LE=16MB) + unc_size(8 LE)`. **45 of 89 decode immediately** with stock `lzma.decompress()`; the remaining 44 (incl. `Glyphs/FE_*.dat` font atlases, `ObjMorphData.dat`) use a second header variant still to be characterised. |
| `.glsl` shaders | 76 | 0.23 MB | **Plaintext GLSL** — directly usable |
| `.amct` textures | 103 | 78.6 MB | Custom container, magic `TCMA` (byte-reversed `AMCT`), CRN codec compiled in → almost certainly compressed, not encrypted |
| `.ama` speech | 514 | 2.6 MB | Custom container, constant 8-byte magic `d4768e4b 00000100`; **not** decodable by any stdlib codec |
| `.IAP` packs | 27 | 3.01 MB | Header `\0IAP` + `02 00 00 00` + `08 06` + 8 per-file bytes, then ciphertext. Genuinely encrypted; almost certainly the localised text. |

Verification: `assets/Data/Additional/Phrases_main.dat` (13,029 B) → LZMA → **53,000 bytes decoded with stock library, no key**. The decoded content is a table of little-endian u32 phrase indices, not prose.

---

## 5. Permissions Analysis

### 5.1 Declared permissions (14)

| Permission | Justified? | Assessment |
|---|---|---|
| `INTERNET` | Yes | Mandatory (ads, backend, multiplayer) |
| `ACCESS_NETWORK_STATE` | Yes | Ad SDK reachability checks |
| `ACCESS_WIFI_STATE` | Yes | Ad SDK / Attribution |
| `VIBRATE` | Yes | Notification + haptics |
| `WAKE_LOCK` | Legitimate | Ad SDK video playback, WorkManager |
| `FOREGROUND_SERVICE` | Legitimate | Ad SDK downloads (Chartboost video cache) |
| `READ_EXTERNAL_STORAGE` | Yes | Custom-music playback (pre-Android 13) |
| `READ_MEDIA_AUDIO` | Yes | Custom-music playback (Android 13+) |
| `VENDING.BILLING` | Yes | IAP |
| `VENDING.CHECK_LICENSE` | Yes | PAIRIP licence check |
| `AD_ID` | Consent-gated | Google Advertising ID |
| `ACCESS_ADSERVICES_{AD_ID,ATTRIBUTION,TOPICS}` | Consent-gated | Privacy Sandbox ads API |
| `array.apphub.permission.BIND_APPHUB_SERVICE` | Vendored | AppLovin AppHub — **not used by this app** |
| `…DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION` | Framework | AndroidX auto-generated, `signature` protection — correct |

**No camera, microphone, contacts, SMS, phone-call, precise location, or `WRITE_EXTERNAL_STORAGE` permissions.** This is a meaningfully tight permission profile for a game, and the audio permission is verifiably used only for reading the user's own playlists (`MediaStore.Audio.Playlists`, `PaybackActivity.java:1502`, `1954`, `2067`).

The app correctly branches on API level: `READ_MEDIA_AUDIO` on Android 13+, `READ_EXTERNAL_STORAGE` below (`PaybackActivity.java:1843-1867`).

### 5.2 Notable absences / capability mismatches

- **No `POST_NOTIFICATIONS`** — despite targeting SDK 35 and actively calling `NotificationManager.notify()`. See §7.1.
- **`play-services-location` 19.0.0 is bundled but no location permission is requested** — dead weight (~1.4 MB + permissions surface with zero functionality). Same for `play-services-fido`, `play-services-drive`, `play-services-places` and the `com.google.android.gms.auth.api.phone` module. These are almost certainly **transitive dependency bloat** from `play-services-base` rather than deliberate choices.
- **`<queries>` for Samsung, Huawei and Amazon app stores** — the app checks for alternative stores (Play Store alternatives). Notable for a Google Play–distributed title; may relate to licensing/traffic or to an abandoned Amazon port.

---

## 6. Advertising & Monetisation

### 6.1 Mediation architecture

The app ships an unusually heavy advertising stack — **eight distinct mediation/integration layers**:

| Layer | SDK | Version | Integration |
|---|---|---|---|
| Google Mobile Ads | `play-services-ads` | 24.5.0 | **Direct** (app calls `MobileAds`, `InterstitialAd`, `RewardedAd`) |
| AdMob Mediation adapters | admob, applovin, adcolony, chartboost, ironsource, pangle, unity, vungle | — | 8 networks under AdMob |
| IronSource LevelPlay | `com.ironsource.*` (obfuscated → `com.json.*`) | 8.9.1 | **Direct** (`IronSource.onResume/onPause`) + Ad Quality SDK 7.25.2 |
| Unity Ads + Unity Ads Mediation | `com.unity3d.*` | 4.15.0 / 8.9.1 | Mediation layer (LevelPlay repackaged) |
| AppLovin MAX | `com.applovin.*` | 12.1.0 | Mediation layer + debug UI (13 `MaxDebugger*Activity` entries) |
| ByteDance Pangle | `com.bytedance.sdk.openadsdk` | 7.3.0.4 | **Direct** (consent API called from app code) |
| Chartboost Monetization | `com.chartboost.sdk` | 9.8.3 | Adapter |
| Vungle / Liftoff | `com.vungle.ads` | 7.5.0 | Adapter |
| AdColony (Jirbo) | `com.adcolony.sdk` | 4.8.0 | Adapter |
| Unity Monetization (legacy) | `com.unity3d.services.ads` | — | Adapter |
| IAB Open Measurement | `com.iab.omid` | 1.5.3 | Viewability |
| TikTok App Events | `com.tiktok.appevents` | 1.3.7 | Attribution |
| APMInsight (Npth) | `com.apm.insight` | unknown | APM / crash reporting (ByteDance lineage) |
| Taboola + Nimbus (native demand) | via MAX | 2.0.0 / 1.0.0.0 | Content-wall monetisation |

**Measured consequence:** **37.5 MB of the 157 MB base APK (24%) is DEX**, and the great majority of that is advertising/mediation SDK code. The actual game glue is roughly 2,700 lines.

The app's own priority tables confirm an 11-slot video-ad and 6-slot interstitial waterfall selected at runtime:

```java
final int EVideoAdType_{AdColony,Vungle,Flurry,Heyzap,AppLovin,InMobi,Apple,Unity,Chartboost,UnityBackfill,AdMob,Max};
int[] PROFILE_SHARED_ad_priority_video = {2, 4, 1, 0, 2, 0, 0, 4, 1, 3, 11};
```

`Flurry`, `Heyzap`, `InMobi` and `Apple` are **vestigial constants** — no such SDKs are present. They are leftovers from the shared native engine. `HW_LogEvent`, `HW_BeginTimedEvent`, `HW_EndTimedEvent`, `HW_ShowMoreApps`, `HW_CanShowMoreApps` and `GMS_invitePlayers` are all **empty method bodies** — the first-party analytics and cross-promotion hooks the engine expects were never implemented in Java.

### 6.2 Consent handling — a real compliance defect

Google's UMP integration is implemented correctly and in good faith:

- `requestConsentInfoUpdate` → `loadAndShowConsentFormIfRequired` on first run (`PaybackActivity.java:823`, `869`).
- `ANDROID_ShowPrivacyOptions()` opens the privacy-options form on demand (`:799`).
- `UpdateOnlinePermission()` (`:718-747`) parses the raw IAB TCF strings from `SharedPreferences` — `IABTCF_PublisherConsent`, `IABTCF_PurposeConsents`, `IABTCF_VendorConsents` (Google vendor ID 755), `IABTCF_PurposeLegitimateInterests`, `IABTCF_VendorLegitimateInterests` — and correctly gates the multiplayer feature on purposes 1/3/4 (consent) and 2/7/9/10/11 (legitimate interest). This is a well-implemented hand-rolled TCF check.

**However**, immediately before AdMob initialisation the app unconditionally force-grants consent to ByteDance:

```java
// PaybackActivity.java:677-678  (inside initializeMobileAdsSdk())
PangleMediationAdapter.setGDPRConsent(1);   // 1 = CONSENT_STATUS_GRANTED
PangleMediationAdapter.setPAConsent(1);     // 1 = granted (PIPL / User-Agent "PA")
```

This runs on **every launch, unconditionally, before and regardless of the actual UMP/TCF state**. Consequences:

- Pangle/ByteDance ad traffic (including the `apm.insight` APM SDK and TikTok App Events, which share the ByteDance data path) is treated as fully consented even when the user has declined in the CMP, is in a restricted region, or is under an age gate.
- This directly contradicts the `setTagForUnderAgeOfConsent(false)` declaration and the app's own TCF evaluation elsewhere.
- ByteDance/Pangle is **not** a Google-certified CMP partner, so it cannot rely on the Google UMP result; it requires its own independently-obtained consent. Hardcoding it defeats the purpose of the CMP that the rest of the app implements correctly.

**This is the single most significant privacy finding in the report** — a GDPR/ePrivacy and PIPL exposure, and a plausible basis for a regulatory complaint.

### 6.3 Ad/privacy-adjacent identifiers

- AdMob Application ID `ca-app-pub-3936847361950191~4276002866`; two ad units: `/2318302655` (interstitial) and `/5822417485` (rewarded).
- AppLovin SDK key embedded in the manifest (expected for this SDK, but it is a live secret in plaintext).
- `TelephonyManager` is used **only** for `getSimCountryIso()`/`getNetworkCountryIso()` in `ANDROID_isChatAllowed()` (`PaybackActivity.java:1272-1287`), which returns `false` for `GB` — a legacy content/geography gate. **No IMEI, IMSI, `getDeviceId()`, Android ID or advertising-ID harvesting was found in first-party code.**
- `ANDROID_getCountryCode()` returns `Locale.getDefault().getCountry()`.
- `play-services-location` is bundled but unusable (no permission).

---

## 7. Security & Robustness Findings

### 7.1 HIGH — Client-side-only IAP entitlement (no purchase verification)

```java
// PaybackActivity.java:423-425
private boolean isPurchaseValid(Purchase purchase) {
    return purchase.getPurchaseState() == 1;      // "PURCHASED" — that is the entire check
}

// :1339-1369  handlePurchase()
iapPurchased[i2] = true;                          // entitlement granted locally
```

`handlePurchase()` performs **no signature verification, no `PlayIntegrity`/`PlayIntegrityToken`, no backend receipt validation, no nonce, no obfuscated-account-binding**. The only validation is Google's own `Purchase.getPurchaseState()`.

Combined with:
- `allowBackup="true"` (save data extractable via `adb backup` on many devices),
- locally-computed entitlements in `iapPurchased[]` handed straight to native code via `GMS_boughtIAP()`,
- `validateExpansionFiles()` returning a **constant `true`**,
- and PAIRIP only verifying the *signing certificate*, not save integrity,

…a rooted device (or a hookable billing client via Frida/Xposed) can trivially unlock `coin_unlimited`, `noads` and `coin_doubler` without payment, or edit local save state. This is a revenue-protection weakness rather than a user-facing vulnerability, but it is a straightforward one to fix: route `Purchase.getPurchaseToken()` through a backend that calls the Play Developer API.

**Fix:** server-side receipt validation + Play Integrity API verdicts; set `allowBackup="false"`; sign save blobs.

### 7.2 HIGH — Cleartext traffic globally enabled

```xml
<application android:usesCleartextTraffic="true" ... >
```

For apps targeting SDK 28+, cleartext is disabled by default; this manifest attribute **re-enables it globally**, overriding any per-domain opt-out. Note also that a `res/xml/network_security_config.xml` **is present in the APK but is never referenced** by the manifest (`networkSecurityConfig` attribute absent) — dead configuration that suggests the intent was there but was never wired up.

All four first-party endpoints are hardcoded in the native library and are **plain HTTP**:

```
http://www.apex-designs.net/pb2_invite.php?game_id=%s        ← referral link (user identifier)
http://www.apex-designs.net/privacy_policy.html               ← privacy policy over HTTP
http://www.apex-designs.net/payback2_faq_android.html         ← FAQ over HTTP
http://www.apex-designs.net/pb2scripts_2106_14_android        ← remote config/scripts over HTTP
https://www.apex-designs.net/pb2scripts_2106_14_android       (HTTPS variant also present)
```

And the Java layer exposes an unrestricted GET helper callable from native code:

```java
// PaybackActivity.java:2173-2187
public String httpGet(String str) {
    HttpURLConnection c = (HttpURLConnection) new URL(str).openConnection();
    if (c.getResponseCode() == 200) { ... char-by-char String concatenation (O(n²)) ... }
    return null;                       // all exceptions silently swallowed
}
```

No timeouts, no TLS pinning, no scheme restriction, exceptions discarded.

**Impact:** a network-position attacker can read and modify the invite/referral identifier in transit, and can **tamper with the remote script bundle** (`pb2scripts_*`) that drives game configuration — a MITM-to-code-execution-in-app vector. Serving the **privacy policy over plain HTTP** is independently problematic for GDPR transparency obligations.

**Fix:** drop `usesCleartextTraffic`, wire up the `networkSecurityConfig`, and move all endpoints to HTTPS.

### 7.3 MEDIUM — Hardcoded OAuth credentials in assets

`assets/oauth_consumer.properties` ships live credentials:

```properties
twitter.com.consumer_key        = YZdcHFbNgAqQWnrYh3GMhJNiA
twitter.com.consumer_secret     = b1LgpMqOjJhbquh1Hqzq08CXYvXqwtN1YwvvKwjofFfvUzflRQ
graph.facebook.com.consumer_key  = 769638416428708
graph.facebook.com.consumer_secret = 39010a0ed01e58dfa1ef19bf0e0200b8
```

No first-party code reads this file (the current share path uses `ACTION_SEND` / `ACTION_SENDTO` and a WebView), so these are almost certainly **dead leftovers** from the original Payback social-sharing implementation. They should still be treated as compromised and **revoked**:

- A leaked **Facebook app secret** lets an attacker impersonate the app to the Graph API.
- A leaked **Twitter OAuth 1.0a consumer secret** allows full 3-legged impersonation of the app's identity.

Credentials of any kind must never be in client code regardless of whether they are currently used.

### 7.4 MEDIUM — WebView: weak host validation + crash bug + third-party redirect

```java
// WebViewActivity.java:29-30
webView.getSettings().setJavaScriptEnabled(true);
webView.loadUrl("https://www.facebook.com/sharer/sharer.php?u=http://bit.ly/payback_2");

// :50-51
public boolean shouldOverrideUrlLoading(WebView webView, String str) {
    if (Uri.parse(str).getHost().endsWith("facebook.com")) {   // ← NPE + suffix match
```

Four distinct problems:

1. **`Uri.parse(str).getHost()` can return `null`** (for `mailto:`, `about:blank`, `data:`, `intent:` URIs) → **unhandled NullPointerException → app crash**. A reachable DoS: any redirect to a scheme without a host kills the app.
2. **`.endsWith("facebook.com")` is a suffix match**, not a hostname comparison. `https://evil-facebook.com`, `https://notfacebook.com`, and `https://attacker.facebook.com.example.org` all satisfy it. Should be `.equals("facebook.com")` with an explicit scheme check.
3. **`shouldOverrideUrlLoading(WebView, String)` is the deprecated pre-API-24 overload.** On Android 7.0+ the `WebResourceRequest` variant is preferred, and the string overload's invocation for main-frame navigations is inconsistent across API levels — meaning the **share-completion reward detection may simply never fire on modern Android**, so rewarded shares break (or, conversely, if the reward is granted by another path, it can be spoofed).
4. **The share target is a `bit.ly` short link over plain HTTP** (`http://bit.ly/payback_2`) embedded as a query parameter to Facebook. The bit.ly destination is controlled by a third party and can be repointed at any destination at any time, without notice to the developer.

Positive: **no `addJavascriptInterface`** is used, so there is no JS-bridge RCE surface. `android.webkit.WebView.EnableSafeBrowsing=true` is present but is **nested inside the IronSource `TestSuiteActivity`** element (`AndroidManifest.xml:232`) rather than at application level, so it has **no effect on the app's own WebViews** — likely a copy-paste error in the manifest.

### 7.5 MEDIUM — Unvalidated deep-link URL passed to native code

```xml
<intent-filter>
    <data android:scheme="payback2"/>
    <action android:name="android.intent.action.VIEW"/>
    <category android:name="android.intent.DEFAULT"/> <category android:name="android.intent.BROWSABLE"/>
</intent-filter>
<intent-filter android:autoVerify="true">
    <data android:scheme="http" android:host="www.apex-designs.net" android:path="/pb2_invite.php"/>
    ...
```

`PaybackActivity` is exported and accepts any `payback2://` URI from any app on the device:

```java
// PaybackActivity.java:852-857
receivedURL = null;
Uri data = getIntent().getData();
if (data == null || !data.isHierarchical()) return;
receivedURL = getIntent().getDataString();     // no scheme/host/path validation
```

This raw, attacker-supplied string is returned verbatim to native code through `ANDROID_getReceivedURL()` and consumed by the invite parser (`payback2://invite=` is a native-side pattern). Depending on what the native handler does with the value, this is a candidate for **arbitrary URL open, reward spoofing, or parser-level memory-safety issues** in a native handler. Requires native-side dynamic analysis to fully bound; the Java side provides **zero input validation**.

Also note the App Link filter uses **`http`, not `https`**, so `autoVerify` on an HTTP host is of limited value (Digital Asset Links verification itself uses HTTPS).

### 7.6 LOW/MEDIUM — Notification feature is non-functional on Android 13+

The app schedules reminder notifications via `AlarmManager` and posts them via `NotificationManager.notify()` (`ReminderService.java:44`, `NotificationQueue.java:113`), but **never declares `android.permission.POST_NOTIFICATIONS`** and never requests it at runtime, despite `targetSdkVersion 35`.

Per Android 13+ behaviour: *"the OS silently drops notifications for the app"* — no exception, no visible symptom. So the entire "daily challenge / coins given / unlocked" reminder system **does nothing** on any device on Android 13 or newer (with the single exception of devices upgraded from Android 12, where the permission may have been auto-pre-granted).

Two further defects in this path:

- `PendingIntent.getBroadcast(...)` at `ReminderService.java:43` and `NotificationQueue.java:110` is created **without `FLAG_IMMUTABLE`**. On API 31+ this throws `IllegalStateException`; below 31 it creates a **mutable PendingIntent**, which is a recognised privilege-escalation vector for intent redirection.
- `taskStackBuilderCreate.getPendingIntent(0, 201326592)` uses `FLAG_ACTIVITY_NEW_TASK | FLAG_ACTIVITY_CLEAR_TASK` on a **fixed request code (0)**, so concurrent notifications collide.

### 7.7 LOW — Dead code and stubbed subsystems

The following are declared and wired into the native engine's ABI, then do nothing:

| Member | Behaviour |
|---|---|
| `validateExpansionFiles()` | `return true;` — DLC integrity check is a stub |
| `ANDROID_getExpansionFileLocation()` | `return "";` — no expansion/DLC download support |
| `ANDROID_requestRetryDownload()` | empty body |
| `HW_LogEvent`, `HW_BeginTimedEvent`, `HW_EndTimedEvent`, `HW_RestartTimedEvent`, `HW_LogEventWithCountry`, `HW_LogEventWithResult` | all empty — first-party analytics never fires |
| `HW_ShowMoreApps`, `HW_CanShowMoreApps` (returns `false`) | cross-promotion disabled |
| `GMS_invitePlayers(int)` | empty — Play Games party invites unimplemented |

The DLC/expansion system was evidently removed or never shipped, leaving `fExpansionDownloadProgress`, `iExpansionDownloadPauseReason`, `iExpansionDownloadFailReason` and the `XAPKFile` class as dead weight, while the engine still holds 34.6 MB of arena for streaming.

### 7.8 LOW — Code robustness

- **`PaybackActivity.instance` static singleton** (`public static PaybackActivity instance`) is never nulled in `onDestroy` — a classic Activity-leak pattern. `WebViewActivity` and `ReminderService` both dereference it.
- **`httpGet` builds its response via `str2 = str2 + (char)i`** inside a `read()` loop → **O(n²)** string concatenation, a reliable ANR/memory risk for any non-trivial response.
- **`NotificationQueue.writeNotifications()`** never closes its `DataOutputStream` in a `finally` block, and reads/writes `openFileInput`/`openFileOutput` with no file locking beyond an in-process `Semaphore` — `ReminderService` (main thread) and the Activity can interleave.
- **`AlarmManager.set(0, ...)`** uses `RTC` rather than `RTC_WAKEUP` and is inexact, so reminder notifications will be delayed by Doze on modern devices.
- **`setAudioStreamType(3)`** (`:2065`) — deprecated since API 26 in favour of `AudioAttributes`.
- **`android.preference.PreferenceManager`** (deprecated framework API) used for the TCF string store.
- **Hardcoded `FLAG_ACTIVITY_NEW_TASK`-style constants** as decimal literals (`524288`, `67108864`, `201326592`) — compiler-safe but hard to audit; magic numbers should be named constants.
- **Log output is benign** — the only `Log.*` calls are four `Log.e` calls about cloud-snapshot failures. No PII, tokens or identifiers are logged.

### 7.9 Exported components — clean

Five exported components, all appropriately constrained:

| Component | Guard |
|---|---|
| `PaybackActivity` | launcher (necessarily exported) — **see §7.5** |
| `RevocationBoundService` | `signature`-level `REVOCATION_NOTIFICATION` permission ✓ |
| `SystemJobService` | `BIND_JOB_SERVICE` ✓ |
| `DiagnosticsReceiver` | `DUMP` ✓ |
| `ProfileInstallReceiver` | `DUMP` ✓ |

No `debuggable`, no `testOnly`, no custom `ContentProvider` with `grantUriPermissions`, no unpermissioned broadcast receivers. **No dangerous exported surface.**

---

## 8. Backend & Network Surface

### 8.1 First-party endpoints (from native strings)

| URL | Purpose | Scheme |
|---|---|---|
| `http://www.apex-designs.net/pb2scripts_2106_14_android` | Remote script/config bundle | ⚠️ HTTP |
| `https://www.apex-designs.net/pb2scripts_2106_14_android` | Same, HTTPS variant | HTTPS |
| `http://www.apex-designs.net/pb2_invite.php?game_id=%s` | Referral / invite links | ⚠️ HTTP |
| `http://www.apex-designs.net/privacy_policy.html` | Privacy policy | ⚠️ HTTP |
| `http://www.apex-designs.net/payback2_faq_android.html` | In-app FAQ | ⚠️ HTTP |

### 8.2 Backend API shape

The engine's networking layer is named **AGAPI** (`AGAPI_Net: Error: Unable to connect to server`) and speaks to a PHP backend via templated URLs:

```
%s/%c.php?a=%d&b=%lli&c=…              generic command channel (int, long long, string)
%s/%c.php?challenge_id=%d&player_id=…  challenge + player identity
%s/a.php                                generic ads/analytics
%s/a_us.php                              US-region variant
%s/a_we.php                              Western-Europe variant
%s/cp.php?a=%d&b=%d&c=%d&d=%d&e=%d&f=%d&g=%d    7-integer telemetry payload
```

The `%s` base URL is supplied at runtime (not present as a literal in the binary), which is a mild protection. Note `pb2scripts_2106_14_android` is version-pinned to build **2106.14** while this APK is **2.106.16** — the remote script bundle trails the client by two patch versions.

### 8.3 Third-party endpoints observed

Pangle: `api16-access-sg.pangle.io`, `api16-access-ttp.tiktokpangle.us`, `lf-static.tiktokpangle-cdn-us.com`, `sf16-static.i18n-pglstatp.com`
AdColony: `adc3-launch.adcolony.com/v4/launch`, `wd.adcolony.com/logs`, `adc-ad-assets.adtilt.com`
Chartboost: `ssp-events.chartboost.com/track/sdk`, `live.chartboost.com`
AppLovin: `dash.applovin.com`, `assets.applovin.com`
Google: `pagead2.googlesyndication.com`, `goo.gle/admob-*`, `play.google.com/store/apps/details?id=`
Taboola: `api.taboola.com`
Facebook (share): `www.facebook.com/sharer/sharer.php`, `bit.ly/payback_2`

No hardcoded IP addresses were found.

### 8.4 Notable positive control

`fetchServerTimestamp` in the native library indicates a **server-time authority check** — the client compares server time against device time, a standard defence against clock manipulation for time-gated game content.

---

## 9. Third-Party Dependency Inventory

Full inventory across **14 top-level namespaces** and 20,562 decompiled classes. Headline versions:

**Advertising / mediation**
`play-services-ads` 24.5.0 · `play-services-ads-identifier` 18.2.0 · `androidx.privacysandbox:ads-adservices` 1.0.0-beta05 · AppLovin SDK **12.1.0** / MAX **12.1.0.0** · AppLovin ExoPlayer fork **2.15.1** · IronSource LevelPlay **8.9.1** (obfuscated to `com.json.*`) + Ad Quality SDK **7.25.2** · Unity Ads **4.15.0** / Unity Ads Mediation **8.9.1** · Pangle **7.3.0.4** + Gecko preload `2.0.3-rc.9-pangle` · Chartboost **9.8.3** · Vungle **7.5.0** · AdColony **4.8.0** · IAB OMID **1.5.3** · TikTok App Events **1.3.7** · Taboola MAX adapter 2.0.0 · Nimbus MAX adapter 1.0.0.0

**Google Play services**
`base` 18.5.0 · `games` 23.1.0 · `auth` 20.7.0 · `drive` 17.0.0 · `fido` 20.1.0 / 20.0.1 · `location` 19.0.0 · `measurement` 20.1.2 · `datatransport` 3.1.8 · UMP **3.2.0** · Billing **7.0.0** · ExoPlayer **2.18.7** · Gson 2.8.6 · Protobuf, Guava

**Runtime / tooling**
Kotlin stdlib 2.1.0 · coroutines 1.7.1 · kotlinx-serialization · OkHttp 4.10.0 · Okio 1.5.1 · AndroidX core 1.10.1, appcompat 1.6.1, activity 1.7.1, work 2.7.0, room 2.2.5, webkit 1.11.0-alpha02, browser 1.8.0, datastore 1.0.0, profileinstaller 1.3.0, emoji2 1.2.0 · core-library desugaring (`desugar_jdk_libs_configuration:2.1.5`, min-api 23)

**Unknown / unversioned**
`com.apm.insight` (APMInsight / Npth APM-crash SDK, 111 classes + 2 native libs) — **no version string anywhere in the APK**; `com.pgl.ssdk.*` (Pangle SS SDK); several obfuscated Pangle internals; `androidx.lifecycle` and `androidx.arch.core:core-runtime` version files are **corrupted** (they contain a Gradle error string instead of a version).

### Dependency-health concerns

- **Three separate mediation layers** (AdMob, LevelPlay, AppLovin MAX) plus Unity's repackaged LevelPlay — four overlapping waterfalls. This is high complexity for high SDK-maintenance cost and high crash surface.
- **LevelPlay is simultaneously obfuscated to `com.json.*`** — an unusual choice that defeats crash-reporting symbolication, per-method stack-trace mapping, and any static security scanning.
- **SDKs with no call site in app code** (AppLovin MAX, Unity Ads, Chartboost, Vungle, AdColony, APMInsight, TikTok App Events, Unity SCAR) are bundled but never invoked directly — they run via server-side waterfall configuration. The full AppLovin MAX **debug UI is shipped in the production build** (13 `MaxDebugger*Activity` entries), which is inappropriate for release.
- **13 AdMob mediation adapters + 3 mediation SDKs** is close to the practical maximum; each is a separate native code path and a separate failure mode.
- Several SDKs are end-of-life or superseded (AdColony 4.8.0 last shipped 2022; Vungle/Liftoff rebranded; `Apple`/`Flurry`/`Heyzap`/`InMobi` constants are stale).

---

## 10. Findings Summary

| # | Severity | Finding | Location |
|---|---|---|---|
| 1 | **High** | ByteDance/Pangle GDPR + PIPL consent hardcoded to "granted" on every launch, bypassing the CMP the app otherwise implements correctly | `PaybackActivity.java:677-678` |
| 2 | **High** | IAP entitlements granted client-side with no receipt/signature/Integrity validation; `validateExpansionFiles()` returns constant `true`; `allowBackup=true` | `PaybackActivity.java:423-425, 1339-1369, 403-405` |
| 3 | **High** | `usesCleartextTraffic="true"` globally; privacy policy, FAQ, invite links and remote script bundle all over plain HTTP; unreferenced `network_security_config.xml`; unrestricted `httpGet()` with no timeouts | `AndroidManifest.xml:61`, native strings, `PaybackActivity.java:2173` |
| 4 | **Medium** | Twitter + Facebook OAuth consumer key/secret committed in `assets/oauth_consumer.properties` | `assets/oauth_consumer.properties` |
| 5 | **Medium** | WebView `Uri.getHost().endsWith("facebook.com")` → NPE crash on null host; suffix-match bypass; deprecated `shouldOverrideUrlLoading` overload breaks share-reward detection; `bit.ly` HTTP share target; SafeBrowsing meta-data misplaced inside an activity | `WebViewActivity.java:50-51, 29-30`; `AndroidManifest.xml:232` |
| 6 | **Medium** | Exported activity accepts arbitrary `payback2://` URIs and passes the unvalidated string to native code; App Link filter uses `http` | `AndroidManifest.xml:74-85`, `PaybackActivity.java:852-857` |
| 7 | **Medium** | No `POST_NOTIFICATIONS` permission while calling `notify()` → reminder feature silently dead on Android 13+ | manifest, `ReminderService.java:44` |
| 8 | **Medium** | `PendingIntent` created without `FLAG_IMMUTABLE` (crashes on API 31+, escalation vector below) | `ReminderService.java:43`, `NotificationQueue.java:110` |
| 9 | **Low** | ~78 MB of native libs, ~69 MB of it zero padding from an oversized `.bss` reservation | `libpayback2.so` |
| 10 | **Low** | Dead/stubbed subsystems: DLC download, first-party analytics, cross-promo, Play Games invites | `PaybackActivity.java:348-405` |
| 11 | **Low** | `httpGet()` O(n²) string building; Activity singleton leak; unclosed `DataOutputStream`; deprecated `setAudioStreamType` | `PaybackActivity.java:2179-2184` |
| 12 | **Low** | PAIRIP allowlists a second (`ALLOWLISTED_SIG`) — likely the Play debug/upload key — weakening tamper detection | `com/pairip/SignatureCheck.java:14` |
| 13 | **Info** | Developer build path leaked: `/Users/james/Documents/AndroidStudio/Payback2/...` | `libpayback2.so` |
| 14 | **Info** | Unused permission-bearing SDKs bundled (`play-services-location`, `-fido`, `-drive`, `-places`, AppLovin AppHub) | `AndroidManifest.xml`, package list |
| 15 | **Info** | AppLovin MAX production **debug UI** shipped in release build | `AndroidManifest.xml:119-133` |

**Explicitly NOT found:** dynamic code loading (`DexClassLoader` / `PathClassLoader` on downloaded payloads), reflection-based payload staging, native exploitation frameworks, root/su detection used offensively, obfuscated C2 infrastructure, hidden network listeners, clipboard or accessibility-service abuse, screen/keylogging, or any data-exfiltration path in first-party code. The heavy ad-SDK footprint reflects aggressive monetisation, not malicious behaviour.

---

## 11. Recommendations

**Privacy & compliance (highest priority)**
1. Remove the hardcoded `PangleMediationAdapter.setGDPRConsent(1)` / `setPAConsent(1)` calls; derive Pangle consent from the actual TCF/UMP state and from a dedicated Pangle consent flow, and honour age gating and regional restrictions.
2. Move the privacy policy, FAQ and invite links to HTTPS; complete a GDPR/UK-GDPR data inventory covering the ad and attribution SDKs, and verify each SDK's participation in the IAB TCF vendor list.

**Security**
3. Implement server-side IAP receipt validation (Play Developer API) and bind entitlements to a server-side account; adopt Play Integrity API for tamper signals.
4. Remove `android:usesCleartextTraffic="true"`; wire up `android:networkSecurityConfig` with the existing `res/xml/network_security_config.xml`; migrate all endpoints to HTTPS.
5. Revoke and rotate the exposed Twitter and Facebook credentials; delete the file entirely.
6. Replace `.endsWith("facebook.com")` with an exact host match plus scheme check, null-guard `getHost()`, implement the `WebResourceRequest` overload, and move the `<meta-data android.webkit.WebView.EnableSafeBrowsing>` to application level.
7. Validate deep-link URIs (scheme, host, path allowlist) before passing anything to native code; switch the App Link filter to `https`.
8. Add `FLAG_IMMUTABLE` to all `PendingIntent` creation; declare and request `POST_NOTIFICATIONS` on API 33+.
9. Set `allowBackup="false"`.

**Hygiene & performance**
10. Strip the embedded build path with `-ffile-prefix-map`; narrow PAIRIP's `ALLOWLISTED_SIG`.
11. Remove the 34.6 MB `.bss` padding by resizing the arena to measured peak usage — this alone cuts ~69 MB of installed size.
12. Collapse to **one** mediation layer (AppLovin MAX or LevelPlay), drop end-of-life adapters (AdColony, Vungle), remove unused Play services modules, and exclude the MAX debug UI from release builds.
13. Fix the deprecation set: `AlarmManager` flags, `setAudioStreamType`, `PreferenceManager`, `httpGet()` string building, Activity lifecycle/static-instance handling.

---

## Appendix A — Methodology

| Step | Tool / method |
|---|---|
| Container & `.apkm` split extraction | Python `zipfile` (custom structure/census scripts) |
| Manifest & resource decoding | `apktool 3.0.3` (`-s`, resources only) |
| DEX → Java decompilation | `jadx` (20,562 files; 140 non-fatal errors on obfuscated code) |
| String extraction | Custom DEX string-table parser (`string_ids` + ULEB128 + MUTF-8) → 341,887 strings |
| Native analysis | `strings`, custom ELF64 section-header + per-section Shannon-entropy parser |
| Entropy forensics | Shannon entropy per byte, 7.91–7.96 bits/byte on `\x00IAP` asset blobs |
| SDK inventory | `META-INF/*.version` markers, `BuildConfig.VERSION_NAME`, Kotlin module markers, `*.properties`, Kotlin `@Metadata` |
| Signature verification | `keytool -printcert` on `META-INF/BNDLTOOL.RSA` |
| Notification behaviour | Verified against Android 13+ platform documentation |

**Limitations.** This is static analysis. The 34.6 MB `.bss` arena, the `\x00IAP` asset format, the AGAPI protocol and the Photon session layer were not decrypted or decompiled — findings about them are limited to symbol, string and structure evidence. Findings #5 and #6 in particular should be confirmed with runtime/instrumented testing (Frida) against the native handlers before being treated as exploitable. No `adb`-based dynamic testing, traffic capture, or interaction with the developer's live backend was performed.

---

*Report generated 2026-10-15. All findings are reproducible from the supplied `.apkm`.*
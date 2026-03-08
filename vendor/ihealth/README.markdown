# Vendor iHealth artifacts

This folder contains **derived** artifacts used for analysis (e.g. extracted native libraries).

## MyVitals APK

The raw vendor APK (e.g. `iHealth MyVitals_4.13.1_APKPure.apk`) is **not** stored in git:

- GitHub rejects files >100MB.
- APK redistribution may be restricted by the app’s license/copyright.

### Recommended sharing (when permitted)

If you are allowed to redistribute the APK to collaborators, publish it as a **GitHub Release asset** for this repo (tag like `vendor-myvitals-4.13.1`) and include the SHA256 in the release notes.

Compute SHA256:

```sh
shasum -a 256 "iHealth MyVitals_4.13.1_APKPure.apk"
```

### If you are not permitted to redistribute

Store only:

- the app name/version,
- where to obtain it (official source),
- and the SHA256 hash for verification.

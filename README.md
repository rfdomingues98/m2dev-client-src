# Client Source Repository

This repository contains the source code necessary to compile the game client executable.

## How to build

> cmake -S . -B build
>
> cmake --build build

---

## 📋 Changelog

### 🐛 Bug Fixes
* **Debug mode:** Fly effects are now registering when using Debug mode.
* **Fix effect rendering in low opacity models:** Effects now appear normally on semi-transparent meshes.
* **Fly targeting fixed for buff/healing skills:** Fixed an issue where fly target effect would render in the buffer's selected target even if the target was unbuffable (if viewing from another client).

<div align="center">

# 📸 Project-FastPic
**An Ultra-Fast Photo Culling Tool Built for Photographers**

![C++17](https://img.shields.io/badge/C%2B%2B17-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white)
![DirectX 11](https://img.shields.io/badge/DirectX_11-107C10?style=for-the-badge&logo=windows&logoColor=white)
![License: GPL v3](https://img.shields.io/badge/License-GPL_v3-blue?style=for-the-badge)

![Download](https://github.com/sumin2391/Project-Fast_PIC/releases/download/v1.0.0/FastPic.v1.0.0.7z)

</div>

<br/>
<!-- ==================== 영문 버전 (기본 닫힘) ==================== -->
<details name="language-switch">
<summary><b>🇺🇸 English Documentation (Click to Expand / Collapse)</b></summary>
<br/>

> **Project-Fast_PIC** is a Windows desktop image viewer designed to browse and cull thousands of high-resolution RAW and JPG photos with zero latency. 
> Built on C++ and DirectX 11 hardware acceleration, it delivers fast processing speeds and focuses on maximizing photo culling efficiency through an intuitive UI and keyboard-centric navigation.

This project was built and evolved upon the minimal project structure and `ImageData` data structure definition created under the initial implementation plan of **Project Kestrel**.

---

## 🚀 Features

### ⚡ Ultra-Fast Image Loading & Processing
- **Lazy Loading:** Instantly constructs the image list using OS file system metadata without opening the image files during folder scans.
- **Background Caching (Pre-fetching):** Proactively decodes photos in the current and next groups into VRAM using multi-threaded background workers.
- **Resolution Control:** Seamlessly switch in real time between optimized (downscaled) loading for rapid browsing and full-resolution loading for detailed inspection.

### 🧭 Efficient Navigation & Viewer
- **Group-Based Browsing:** Groups photos based on capture time (time-gap), allowing you to jump across entire scenes using the Left/Right arrow keys.
- **Safe Wrap-Around Navigation:** Up/Down arrow keys cycle strictly within the current group without accidentally crossing into adjacent groups, maintaining your workflow.
- **Smart Auto-Scroll:** The file list automatically follows your selection during keyboard navigation, while yielding control during mouse wheel scrolling to prevent view conflicts.
- **AF Focus PiP Viewer:** Analyzes EXIF metadata to display the camera's autofocus area in a bottom-right Picture-in-Picture (PiP) loupe window, supporting custom zoom levels and window resizing.

### 📂 Photo Culling & Batch Export
- **Status Labeling:** Assign 3-color traffic light statuses using the <kbd>1</kbd> (Pick), <kbd>2</kbd> (Hold), and <kbd>3</kbd> (Reject) keys, automatically advancing to the next photo.
- **Batch Export:** Copy all photos marked as Pick to a target folder at once (<kbd>E</kbd> key) with automatic filename collision handling.
- Fully supports non-ASCII (including Korean) filenames and directory paths via native wide-string (`std::wstring`) handling.

---

## 🛠️ Tech Stack

- **Language:** C++17
- **Graphics API:** DirectX 11 (Hardware Acceleration)
- **OS API:** Windows API / WIC (Windows Imaging Component)

---

## ⌨️ Shortcuts

| Shortcut | Description |
| :--- | :--- |
| <kbd>Up</kbd> / <kbd>Down</kbd> | Move 1 photo at a time within the current group (wraps around at the end) |
| <kbd>Left</kbd> / <kbd>Right</kbd> | Jump to the previous / next group (burst sequence) |
| <kbd>1</kbd>, <kbd>2</kbd>, <kbd>3</kbd> | Label as Pick, Hold, or Reject (auto-advances to the next photo) |
| <kbd>E</kbd> | Batch export all Picked photos to a selected folder |
| **Mouse Wheel** | Zoom in / out on the main viewer |
| **Left-Click Drag** | Pan zoomed image / Resize the PiP window |
| **Middle-Click (Double-Click)** | Fit to screen (reset zoom and pan position) |

---

## 🤝 Credits & Acknowledgements

- **[Dear ImGui](https://github.com/ocornut/imgui):** UI Framework (MIT License)
- **[LibRaw](https://github.com/LibRaw/LibRaw):** RAW image processing (LGPL 2.1)
- **[stb_image](https://github.com/nothings/stb):** Image loading (MIT / Public Domain)
- **[ExifTool](https://exiftool.org/) by Phil Harvey:** EXIF metadata parsing

## 📄 License

This project is licensed under the **GPL-3.0 License**. See the `LICENSE` file in the repository for details.

</details>

<br/>

<!-- ==================== 한국어 버전 (기본 열림) ==================== -->
<details name="language-switch" open>
<summary><b>🇰🇷 한국어 설명 보기 (클릭하여 펼치기/접기)</b></summary>
<br/>

> **Project-Fast_PIC**은 수천 장의 고해상도 RAW 및 JPG 사진을 지연 없이 열람하고 선별(Culling)하기 위해 설계된 윈도우 데스크톱 이미지 뷰어입니다. 
> C++와 DirectX 11 하드웨어 가속을 기반으로 빠른 처리 속도를 제공하며, 직관적인 UI와 키보드 중심의 네비게이션으로 사진 분류 작업의 효율을 높이는 데 집중했습니다.

이 프로젝트는 **Project Kestrel**의 초기 구현 계획에 따라 생성된 최소한의 프로젝트 구조와 `ImageData` 데이터 구조 정의를 바탕으로 구축 및 발전되었습니다.

---

## 🚀 주요 기능 (Features)

### ⚡ 초고속 이미지 로딩 및 처리
- **지연 로딩 (Lazy Loading):** 폴더 스캔 시 이미지 파일을 직접 열지 않고 OS의 파일 시스템 메타데이터를 활용하여 리스트를 즉시 구성합니다.
- **백그라운드 캐싱 (Pre-fetching):** 다중 백그라운드 스레드를 활용하여 현재 그룹 및 다음 그룹의 사진을 VRAM에 선제적으로 디코딩합니다.
- **해상도 제어:** 빠른 탐색을 위한 최적화(축소) 화소 로딩과 디테일 확인을 위한 원본 화소 로딩을 실시간으로 전환할 수 있습니다.

### 🧭 효율적인 네비게이션 및 뷰어 기능
- **그룹 단위 탐색:** 촬영 시간(Time-gap)을 기준으로 사진을 그룹화하여 방향키(좌/우)로 씬(Scene) 단위를 빠르게 건너뜁니다.
- **안전한 순환 이동:** 방향키(위/아래) 이동 시 다른 그룹으로 넘어가지 않고 해당 그룹 내에서만 순환(Wrap-around)하여 작업 흐름을 유지합니다.
- **자동 스크롤 제어:** 키보드 탐색 시 리스트가 자동으로 시야를 따라가며, 마우스 휠 조작 시에는 자동 스크롤이 해제되어 충돌을 방지합니다.
- **AF Focus PiP 뷰어:** Exif 데이터를 분석하여 카메라의 초점 영역을 우측 하단 돋보기 창(PiP)에 표시하며, 배율 및 창 크기 조절을 지원합니다.

### 📂 사진 분류(Culling) 및 일괄 내보내기
- **상태 라벨링:** 숫자 <kbd>1</kbd>(Pick), <kbd>2</kbd>(Hold), <kbd>3</kbd>(Reject) 키를 통해 3색 신호등 상태를 지정하고 자동으로 다음 사진으로 넘어갑니다.
- **일괄 내보내기 (Export):** Pick 상태로 분류된 사진들만 지정된 폴더로 파일명 충돌 처리와 함께 일괄 복사(<kbd>E</kbd> 키)합니다.
- `std::wstring` 기반의 네이티브 와이드 문자열 처리를 통해 한글 파일명 및 한글 경로를 완벽하게 지원합니다.

---

## 🛠️ 기술 스택 (Tech Stack)

- **Language:** C++17
- **Graphics API:** DirectX 11 (Hardware Acceleration)
- **OS API:** Windows API / WIC (Windows Imaging Component)

---

## ⌨️ 조작 방법 (Shortcuts)

| 단축키 | 기능 설명 |
| :--- | :--- |
| <kbd>위</kbd> / <kbd>아래</kbd> | 현재 그룹 내에서 사진 1장씩 이동 (끝 도달 시 순환) |
| <kbd>좌</kbd> / <kbd>우</kbd> | 이전 / 다음 그룹(연사 묶음)으로 통째로 점프 |
| <kbd>숫자 1, 2, 3</kbd> | Pick(선택), Hold(보류), Reject(제외) 라벨링 (자동 다음 장 이동) |
| <kbd>E</kbd> | Pick으로 지정된 사진들을 지정된 폴더로 일괄 내보내기 |
| **마우스 휠** | 메인 뷰어 사진 확대 / 축소 (Zoom) |
| **좌클릭 드래그** | 확대된 사진 이동 (Panning) 및 PiP 창 크기 조절 |
| **휠 클릭(더블클릭)** | 화면 맞춤 (기본 배율 및 위치로 복귀) |

---

## 🤝 크레딧 및 감사의 글

- **[Dear ImGui](https://github.com/ocornut/imgui):** UI Framework (MIT License)
- **[LibRaw](https://github.com/LibRaw/LibRaw):** RAW image processing (LGPL 2.1)
- **[stb_image](https://github.com/nothings/stb):** Image loading (MIT / Public Domain)
- **[ExifTool](https://exiftool.org/) by Phil Harvey:** EXIF metadata parsing

## 📄 License

이 프로젝트는 **GPL-3.0 License**를 따릅니다. 상세한 내용은 저장소의 `LICENSE` 파일을 참조하세요.

</details>

<br/>
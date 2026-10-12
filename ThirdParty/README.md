ThirdParty 폴더 안내

- LibRaw: RAW 파일 지원용 네이티브 라이브러리. 초기 릴리스에 포함 예정이며, 빌드/배포 시 사전컴파일 바이너리 또는 소스 컴파일을 선택하세요.
- stb_image.h: 썸네일 및 JPG/PNG 디코딩용 경량 헤더 라이브러리.
- pHash: perceptual hash 구현(또는 간단 구현체).
- Dear ImGui: UI 프레임워크 및 DirectX11 + Win32 백엔드 코드.

프로젝트에 외부 라이브러리를 추가할 때는 Visual Studio 프로젝트 속성에서 include/lib 경로를 설정하고, 필요한 경우 네이티브 DLL을 배포 디렉터리에 복사하세요.

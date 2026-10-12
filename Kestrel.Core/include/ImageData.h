#pragma once

#include <string>
#include <ctime>

namespace Kestrel {

struct ImageData {
	// 기본 메타데이터
	std::string filePath;    // 절대 경로
	std::string fileName;    // 파일명
	std::string captureTime; // EXIF 촬영 시간(문자열)
	std::time_t captureTimeT = 0; // 연산용 Unix 시간(초 단위)
	int groupID = -1;        // 연사 그룹 식별 번호

	// 상태 플래그
	bool isPicked = false;
	bool isRejected = false;
	bool isHold = false;

	// 썸네일/디코드 관련 메타데이터
	int thumbnailWidth = 0;
	int thumbnailHeight = 0;

	// 향후 확장 필드(초점 점수, 해시 등)
	// float focusScore = 0.0f;
	// uint64_t pHash = 0;
};

} // namespace Kestrel

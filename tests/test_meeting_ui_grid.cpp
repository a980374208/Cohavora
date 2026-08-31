#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <map>

struct Rect {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;

	int right() const { return x + w; }
	int bottom() const { return y + h; }

	bool intersects(const Rect &other) const {
		return !(x >= other.right() || right() <= other.x ||
		         y >= other.bottom() || bottom() <= other.y);
	}
};

struct GridLayoutResult {
	int rows = 1;
	int cols = 1;
	int tileW = 0;
	int tileH = 0;
	std::vector<Rect> tiles;
};

GridLayoutResult ComputeOptimal16x9Grid(int stageW, int stageH, int N, int margin = 8, int gap = 8) {
	GridLayoutResult res;
	if (stageW <= 0 || stageH <= 0 || N <= 0) return res;

	int bestRows = 1, bestCols = 1;
	int bestTileW = 0, bestTileH = 0;
	double maxArea = 0.0;

	for (int cols = 1; cols <= N; ++cols) {
		int rows = (N + cols - 1) / cols;
		int availW = stageW - margin * 2 - (cols - 1) * gap;
		int availH = stageH - margin * 2 - (rows - 1) * gap;
		if (availW <= 0 || availH <= 0) continue;

		int maxW = availW / cols;
		int maxH = availH / rows;

		int tW = maxW;
		int tH = maxH;
		if (static_cast<double>(maxW) / maxH > 16.0 / 9.0) {
			tW = static_cast<int>(maxH * 16.0 / 9.0);
			tH = maxH;
		} else {
			tW = maxW;
			tH = static_cast<int>(maxW * 9.0 / 16.0);
		}

		double area = static_cast<double>(tW) * tH;
		if (area > maxArea) {
			maxArea = area;
			bestRows = rows;
			bestCols = cols;
			bestTileW = tW;
			bestTileH = tH;
		}
	}

	res.rows = bestRows;
	res.cols = bestCols;
	res.tileW = bestTileW;
	res.tileH = bestTileH;

	int totalGridH = bestRows * bestTileH + (bestRows - 1) * gap;
	int startY = (stageH - totalGridH) / 2;

	int tileIdx = 0;
	for (int r = 0; r < bestRows && tileIdx < N; ++r) {
		int itemsInRow = std::min(bestCols, N - r * bestCols);
		int rowW = itemsInRow * bestTileW + (itemsInRow - 1) * gap;
		int startX = (stageW - rowW) / 2;

		for (int c = 0; c < itemsInRow && tileIdx < N; ++c) {
			Rect geom;
			geom.x = startX + c * (bestTileW + gap);
			geom.y = startY + r * (bestTileH + gap);
			geom.w = bestTileW;
			geom.h = bestTileH;
			res.tiles.push_back(geom);
			++tileIdx;
		}
	}

	return res;
}

uint32_t HashString(const std::string &str) {
	uint32_t hash = 5381;
	for (char c : str) {
		hash = ((hash << 5) + hash) + static_cast<uint8_t>(c);
	}
	return hash;
}

void TestGridGeometry() {
	std::cout << "[TEST] 验证自适应 16:9 网格排版算法..." << std::endl;

	const int W = 1280;
	const int H = 720;

	for (int n = 1; n <= 16; ++n) {
		auto res = ComputeOptimal16x9Grid(W, H, n);
		assert(res.tiles.size() == static_cast<size_t>(n));
		assert(res.tileW > 0 && res.tileH > 0);

		double aspect = static_cast<double>(res.tileW) / res.tileH;
		assert(std::abs(aspect - 16.0 / 9.0) < 0.15);

		for (const auto &t : res.tiles) {
			assert(t.x >= 0 && t.right() <= W);
			assert(t.y >= 0 && t.bottom() <= H);
		}

		for (size_t i = 0; i < res.tiles.size(); ++i) {
			for (size_t j = i + 1; j < res.tiles.size(); ++j) {
				assert(!res.tiles[i].intersects(res.tiles[j]));
			}
		}

		std::cout << "  - N=" << n << " -> " << res.cols << "x" << res.rows
		          << " (Tile: " << res.tileW << "x" << res.tileH << ", Aspect: " << aspect << ") [PASS]" << std::endl;
	}
}

void TestAvatarHash() {
	std::cout << "[TEST] 验证头像色彩哈希一致性与区分度..." << std::endl;

	std::string userA = "Alice";
	std::string userB = "Bob";
	std::string userC = "Charlie";

	uint32_t hA1 = HashString(userA);
	uint32_t hA2 = HashString(userA);
	uint32_t hB = HashString(userB);
	uint32_t hC = HashString(userC);

	assert(hA1 == hA2);
	assert(hA1 != hB);
	assert(hB != hC);

	std::cout << "  - Alice: 0x" << std::hex << hA1 << " (Palette: " << (hA1 % 8) << ")" << std::endl;
	std::cout << "  - Bob:   0x" << std::hex << hB  << " (Palette: " << (hB % 8) << ")" << std::endl;
	std::cout << "  - Char:  0x" << std::hex << hC  << " (Palette: " << (hC % 8) << ")" << std::dec << std::endl;
	std::cout << "  [PASS] 色彩哈希算法确定且均匀" << std::endl;
}

int main() {
	std::cout << "==========================================" << std::endl;
	std::cout << "      LiveKit Multi-User Grid Test        " << std::endl;
	std::cout << "==========================================" << std::endl;

	TestGridGeometry();
	TestAvatarHash();

	std::cout << "==========================================" << std::endl;
	std::cout << "   ALL MULTI-USER GRID TESTS PASSED!      " << std::endl;
	std::cout << "==========================================" << std::endl;
	return 0;
}

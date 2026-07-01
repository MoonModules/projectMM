#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/draw.h"           // draw::pixel
#include "core/math8.h"           // Random8

#include <array>
#include <cstdint>
#include <cstring>                // std::strcmp

namespace mm {

// A solved 3D Rubik's cube that scrambles itself, then plays the scramble back in reverse so the
// cube visibly un-mixes turn by turn, and re-scrambles once solved. The cube is a full 6-face model
// (up to 8×8 stickers per face) with the real face/row/column rotations; each frame it is drawn onto
// the LED volume by classifying every in-bounds voxel as belonging to whichever of the six outer
// faces it sits nearest, and colouring it from that face's sticker. Turns play at `turnsPerSecond`;
// `cubeSize` is the order of the cube (2..8 are real cubes, 1 is a degenerate single block); with
// `randomTurning` the cube tumbles through endless random moves instead of solving a stored scramble.
//
// Prior art: MoonLight's RubiksCube effect (E_MoonModules / MoonModules). The cube model
// (init/rotateFace/rotateRow/rotateColumn/rotateFaceLayer and the six face rotations), the packed
// move list + scramble/playback, and drawCube's nearest-face projection with the
// {Red, DarkOrange, Blue, Green, Yellow, White} colour map are reproduced exactly here, written
// fresh on EffectBase + the shared draw primitive. projectMM has no per-cell mapping mask, so every
// in-bounds voxel is treated as mapped (the source's isMapped()-skip and the mapping-driven
// sizeX++/sizeY++/sizeZ++ adjustments are dropped; the projection uses sizeX = max(size.x-1, 1)).
// Author: WildCats08 / @Brandon502 (MoonLight) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
class RubiksCubeEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🧊"; }  // MoonLight origin · 3D-native
    Dim dimensions() const override { return Dim::D3; }

    // Defaults match MoonLight's RubiksCube exactly.
    uint8_t turnsPerSecond = 2;       // 0..20
    uint8_t cubeSize       = 3;       // 1..8 (cube order)
    bool    randomTurning  = false;

    void onBuildControls() override {
        controls_.addUint8("turnsPerSecond", turnsPerSecond, 0, 20);
        controls_.addUint8("cubeSize", cubeSize, 1, 8);
        controls_.addBool("randomTurning", randomTurning);
    }

    // cubeSize / randomTurning changes re-scramble (MoonLight reinitialises on those controls). No
    // heap state to rebuild, so the cheap onUpdate hook is enough — flag a fresh init for the next loop.
    void onUpdate(const char* name) override {
        if (std::strcmp(name, "cubeSize") == 0 || std::strcmp(name, "randomTurning") == 0)
            doInit_ = true;
    }

    void loop() override {
        const lengthType w = width(), h = height(), d = depth();
        if (w <= 0 || h <= 0 || d <= 0 || channelsPerLight() < 3) return;

        Buffer& buf = layer()->buffer();
        const Coord3D dims{w, h, d};

        const uint32_t now = elapsed();

        // (Re-)scramble on a requested re-scramble (doInit_ — set on first run and on control change,
        // once past the initial delay), OR when `step_` is stuck unreasonably far in the future.
        // MoonLight writes the second check as `step - 3100 > now`, but with unsigned `step_` that
        // UNDERFLOWS whenever step_ < 3100 (the first ~3 s of uptime, and after every turn where step_
        // is set to `now`), so it fires init EVERY frame — the cube re-scrambles each tick and the
        // display FLASHES instead of turning one slice at a time. The fix is a wrap-safe SIGNED
        // difference: re-init only when step_ is genuinely more than 3100 ms ahead of now.
        const int32_t ahead = static_cast<int32_t>(step_ - now);   // how far step_ is in the future (signed)
        if ((doInit_ && now > step_) || ahead > 3100) {
            step_ = now + 1000;
            doInit_ = false;
            init(buf, dims, w, h, d);
        }

        // Turn pacing: nothing to do until 1000/turnsPerSecond ms have passed since the last turn.
        if (!turnsPerSecond || now - step_ < 1000u / turnsPerSecond || now < step_) return;

        const Move move = randomTurning ? createRandomMoveStruct(cubeSize, prevFaceMoved_)
                                        : unpackMove(moveList_[moveIndex_]);
        // Playback applies the inverse direction so the scramble unwinds toward solved.
        (cube_.*kRotateFuncs[move.face])(!move.direction, static_cast<uint8_t>(move.width + 1));
        cube_.drawCube(buf, dims, w, h, d);

        if (!randomTurning && moveIndex_ == 0) {
            step_ = now + 3000;   // solved: hold for 3 s, then re-scramble
            doInit_ = true;
            return;
        }
        if (!randomTurning) moveIndex_--;
        step_ = now;
    }

private:
    // --- The cube model -------------------------------------------------------------------------
    struct Cube {
        uint8_t SIZE = 3;
        static const uint8_t MAX_SIZE = 8;
        using Face = std::array<std::array<uint8_t, MAX_SIZE>, MAX_SIZE>;
        Face front, back, left, right, top, bottom;

        void init(uint8_t cubeSize) {
            SIZE = cubeSize;
            for (int i = 0; i < MAX_SIZE; i++)
                for (int j = 0; j < MAX_SIZE; j++) {
                    front[i][j] = 0; back[i][j] = 1; left[i][j] = 2;
                    right[i][j] = 3; top[i][j] = 4; bottom[i][j] = 5;
                }
        }

        void rotateFace(Face& face, bool clockwise) {
            Face temp = face;
            if (clockwise)
                for (int i = 0; i < SIZE; i++) for (int j = 0; j < SIZE; j++) face[j][SIZE - 1 - i] = temp[i][j];
            else
                for (int i = 0; i < SIZE; i++) for (int j = 0; j < SIZE; j++) face[SIZE - 1 - j][i] = temp[i][j];
        }

        void rotateRow(int startRow, int stopRow, bool clockwise) {
            std::array<uint8_t, MAX_SIZE> temp;
            for (int row = startRow; row <= stopRow; row++) {
                if (clockwise)
                    for (int i = 0; i < SIZE; i++) {
                        temp[i] = left[row][i];
                        left[row][i] = front[row][i]; front[row][i] = right[row][i];
                        right[row][i] = back[row][i]; back[row][i] = temp[i];
                    }
                else
                    for (int i = 0; i < SIZE; i++) {
                        temp[i] = left[row][i];
                        left[row][i] = back[row][i]; back[row][i] = right[row][i];
                        right[row][i] = front[row][i]; front[row][i] = temp[i];
                    }
            }
        }

        void rotateColumn(int startCol, int stopCol, bool clockwise) {
            std::array<uint8_t, MAX_SIZE> temp;
            for (int col = startCol; col <= stopCol; col++) {
                if (clockwise)
                    for (int i = 0; i < SIZE; i++) {
                        temp[i] = top[i][col];
                        top[i][col] = front[i][col]; front[i][col] = bottom[i][col];
                        bottom[i][col] = back[SIZE - 1 - i][SIZE - 1 - col]; back[SIZE - 1 - i][SIZE - 1 - col] = temp[i];
                    }
                else
                    for (int i = 0; i < SIZE; i++) {
                        temp[i] = top[i][col];
                        top[i][col] = back[SIZE - 1 - i][SIZE - 1 - col]; back[SIZE - 1 - i][SIZE - 1 - col] = bottom[i][col];
                        bottom[i][col] = front[i][col]; front[i][col] = temp[i];
                    }
            }
        }

        void rotateFaceLayer(bool clockwise, int startLayer, int endLayer) {
            for (int layer = startLayer; layer <= endLayer; layer++) {
                std::array<uint8_t, MAX_SIZE> temp;
                for (int i = 0; i < SIZE; i++) temp[i] = clockwise ? top[SIZE - 1 - layer][i] : bottom[layer][i];
                for (int i = 0; i < SIZE; i++) {
                    if (clockwise) {
                        top[SIZE - 1 - layer][i] = left[SIZE - 1 - i][SIZE - 1 - layer];
                        left[SIZE - 1 - i][SIZE - 1 - layer] = bottom[layer][SIZE - 1 - i];
                        bottom[layer][SIZE - 1 - i] = right[i][layer];
                        right[i][layer] = temp[i];
                    } else {
                        bottom[layer][SIZE - 1 - i] = left[SIZE - 1 - i][SIZE - 1 - layer];
                        left[SIZE - 1 - i][SIZE - 1 - layer] = top[SIZE - 1 - layer][i];
                        top[SIZE - 1 - layer][i] = right[i][layer];
                        right[i][layer] = temp[SIZE - 1 - i];
                    }
                }
            }
        }

        void rotateFront(bool clockwise, uint8_t width) {
            rotateFaceLayer(clockwise, 0, width - 1);
            rotateFace(front, clockwise);
            if (width >= SIZE) rotateFace(back, !clockwise);
        }
        void rotateBack(bool clockwise, uint8_t width) {
            rotateFaceLayer(!clockwise, SIZE - width, SIZE - 1);
            rotateFace(back, clockwise);
            if (width >= SIZE) rotateFace(front, !clockwise);
        }
        void rotateLeft(bool clockwise, uint8_t width) {
            rotateFace(left, clockwise);
            rotateColumn(0, width - 1, !clockwise);
            if (width >= SIZE) rotateFace(right, !clockwise);
        }
        void rotateRight(bool clockwise, uint8_t width) {
            rotateFace(right, clockwise);
            rotateColumn(SIZE - width, SIZE - 1, clockwise);
            if (width >= SIZE) rotateFace(left, !clockwise);
        }
        void rotateTop(bool clockwise, uint8_t width) {
            rotateFace(top, clockwise);
            rotateRow(0, width - 1, clockwise);
            if (width >= SIZE) rotateFace(bottom, !clockwise);
        }
        void rotateBottom(bool clockwise, uint8_t width) {
            rotateFace(bottom, clockwise);
            rotateRow(SIZE - width, SIZE - 1, !clockwise);
            if (width >= SIZE) rotateFace(top, !clockwise);
        }

        // Project the cube onto the LED volume: every in-bounds voxel is coloured by the outer face
        // it sits nearest. (MoonLight's drawCube, with the isMapped()-skip and sizeX++/etc dropped.)
        void drawCube(Buffer& buf, Coord3D dims, lengthType sx, lengthType sy, lengthType sz) const {
            // This effect owns its background: drawCube writes only the SURFACE voxels (the loop has
            // no else for the interior), and a turn moves stickers to new positions, so without a
            // wipe the old pose's stickers linger and the cube accretes garbage — it never settles.
            // One fill per draw (drawCube runs once per turn, not per frame), the sparse-effect idiom.
            draw::fill(buf, {0, 0, 0});
            const int sizeX = MAXi(sx - 1, 1), sizeY = MAXi(sy - 1, 1), sizeZ = MAXi(sz - 1, 1);
            // Integer form of round(coord * (SIZE+1) / size): for non-negative operands round(a/b) is
            // (2a + b) / (2b), which reproduces round(coord*scale) exactly at these magnitudes — no
            // per-voxel float multiply or round() in the hot loop. num = 2·(SIZE+1) is the shared
            // numerator factor; denX/Y/Z = 2·size are the doubled per-axis denominators.
            const int num = 2 * (SIZE + 1);
            const int denX = 2 * sizeX, denY = 2 * sizeY, denZ = 2 * sizeZ;
            const int halfX = sizeX / 2, halfY = sizeY / 2, halfZ = sizeZ / 2;

            // Red, DarkOrange, Blue, Green, Yellow, White.
            const RGB COLOR_MAP[6] = {
                {255, 0, 0}, {255, 140, 0}, {0, 0, 255}, {0, 128, 0}, {255, 255, 0}, {255, 255, 255}};

            for (int x = 0; x < sx; x++)
                for (int y = 0; y < sy; y++)
                    for (int z = 0; z < sz; z++) {
                        const Coord3D led{static_cast<lengthType>(x), static_cast<lengthType>(y), static_cast<lengthType>(z)};
                        const int nX = constrainI((x * num + sizeX) / denX - 1, 0, SIZE - 1);
                        const int nY = constrainI((y * num + sizeY) / denY - 1, 0, SIZE - 1);
                        const int nZ = constrainI((z * num + sizeZ) / denZ - 1, 0, SIZE - 1);
                        const int distX = MINi(x, sizeX - x), distY = MINi(y, sizeY - y), distZ = MINi(z, sizeZ - z);
                        const int dist = MINi(distX, MINi(distY, distZ));

                        if      (dist == distZ && z < halfZ)  draw::pixel(buf, dims, led, COLOR_MAP[front[nY][nX]]);
                        else if (dist == distX && x < halfX)  draw::pixel(buf, dims, led, COLOR_MAP[left[nY][SIZE - 1 - nZ]]);
                        else if (dist == distY && y < halfY)  draw::pixel(buf, dims, led, COLOR_MAP[top[SIZE - 1 - nZ][nX]]);
                        else if (dist == distZ && z >= halfZ) draw::pixel(buf, dims, led, COLOR_MAP[back[nY][SIZE - 1 - nX]]);
                        else if (dist == distX && x >= halfX) draw::pixel(buf, dims, led, COLOR_MAP[right[nY][nZ]]);
                        else if (dist == distY && y >= halfY) draw::pixel(buf, dims, led, COLOR_MAP[bottom[nZ][nX]]);
                    }
        }
    };

    // A move: which face turns, how many layers wide, which direction. Packed into one byte for the
    // stored scramble list (3 bits face, 3 bits width, 1 bit direction).
    struct Move { uint8_t face, width, direction; };

    Move createRandomMoveStruct(uint8_t size, uint8_t prevFace) {
        Move move;
        do { move.face = rng_.below(6); } while (move.face / 2 == prevFace / 2);
        move.width = (size > 2) ? rng_.below(static_cast<uint8_t>(size - 2)) : 0;  // random(cubeSize-2)
        move.direction = rng_.below(2);
        return move;
    }
    static uint8_t packMove(Move m) {
        return static_cast<uint8_t>((m.face & 0b111) | ((m.width << 3) & 0b111000) | ((m.direction << 6) & 0b1000000));
    }
    static Move unpackMove(uint8_t p) {
        Move m;
        m.face = static_cast<uint8_t>(p & 0b111);
        m.width = static_cast<uint8_t>((p >> 3) & 0b111);
        m.direction = static_cast<uint8_t>((p >> 6) & 0b1);
        return m;
    }

    using RotateFunc = void (Cube::*)(bool, uint8_t);
    static constexpr RotateFunc kRotateFuncs[6] = {
        &Cube::rotateFront, &Cube::rotateBack, &Cube::rotateLeft,
        &Cube::rotateRight, &Cube::rotateTop, &Cube::rotateBottom};

    // Build a fresh solved cube, give it a few random whole-cube turns, then scramble it with a
    // stored move list (so playback can reverse it), and draw the scrambled state.
    void init(Buffer& buf, Coord3D dims, lengthType w, lengthType h, lengthType d) {
        cube_.init(cubeSize);
        const int moveCount = cubeSize * 10 + rng_.below(20);

        for (int x = 0; x < 3; x++) {
            if (rng_.below(2)) cube_.rotateRight(1, cubeSize);
            if (rng_.below(2)) cube_.rotateTop(1, cubeSize);
            if (rng_.below(2)) cube_.rotateFront(1, cubeSize);
        }

        const int cappedMoves = (moveCount > kMaxMoves) ? kMaxMoves : moveCount;
        for (int i = 0; i < cappedMoves; i++) {
            Move move = createRandomMoveStruct(cubeSize, prevFaceMoved_);
            prevFaceMoved_ = move.face;
            moveList_[i] = packMove(move);
            (cube_.*kRotateFuncs[move.face])(move.direction, static_cast<uint8_t>(move.width + 1));
        }
        moveIndex_ = static_cast<uint8_t>(cappedMoves - 1);
        cube_.drawCube(buf, dims, w, h, d);
    }

    // Inline integer helpers (MoonLight's MIN/MAX/constrain).
    static int MINi(int a, int b) { return a < b ? a : b; }
    static int MAXi(int a, int b) { return a > b ? a : b; }
    static int constrainI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    static constexpr int kMaxMoves = 100;   // moveList capacity (cubeSize*10 + 0..19 ≤ 99 for size ≤ 8)

    Cube     cube_;                         // 6 × 8 × 8 = 384 bytes — small enough to keep inline
    uint8_t  moveList_[kMaxMoves] = {};     // packed scramble for reverse playback
    uint8_t  moveIndex_ = 0;
    uint8_t  prevFaceMoved_ = 0;
    uint32_t step_ = 0;                     // ms timestamp gating the next turn / hold window
    bool     doInit_ = true;                // request a fresh scramble (first run + on control change)
    Random8  rng_{0x52554249u};             // "RUBI"
};

} // namespace mm

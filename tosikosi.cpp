#include <windows.h>
#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <iomanip> // 時刻表示用

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
}

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "avutil.lib")

// --- デバッグ用ログ ---
void SetupConsole() {
#ifdef _DEBUG
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    SetConsoleOutputCP(CP_UTF8);

    // --- ここから追加：簡易編集モード（QuickEdit）を無効化 ---
    HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);
    DWORD prev_mode;
    GetConsoleMode(hInput, &prev_mode);
    // ENABLE_QUICK_EDIT_MODE を除外し、ENABLE_EXTENDED_FLAGS を立てる
    SetConsoleMode(hInput, (prev_mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
    // --- ここまで ---

    std::cout << "[DEBUG] Console opened (QuickEdit Disabled)" << std::endl;
#endif
}

// --- 共有データ構造 ---
struct VideoFrame {
    std::vector<uint8_t> data;
    double pts;
    int width, height;
};

std::queue<VideoFrame> frame_queue;
std::mutex queue_mtx;
std::condition_variable cv;
bool decoding_finished = false;
const size_t MAX_QUEUE_SIZE = 10;

// --- デコードスレッド (変更なし) ---
void DecodeThread(const char* filename) {
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filename, nullptr, nullptr) < 0) {
        MessageBoxA(nullptr, "Could not open video file.", "FFmpeg Error", MB_OK | MB_ICONERROR);
        exit(-1);
    }
    avformat_find_stream_info(fmt_ctx, nullptr);

    int video_index = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++)
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { video_index = i; break; }

    if (video_index == -1) exit(-1);

    AVCodecParameters* codecpar = fmt_ctx->streams[video_index]->codecpar;
    const AVCodec* codec = avcodec_find_decoder_by_name("libvpx");
    if (!codec) codec = avcodec_find_decoder(codecpar->codec_id);

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codecpar);

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "alpha_mode", "1", 0);
    avcodec_open2(codec_ctx, codec, &opts);
    av_dict_free(&opts);

    SwsContext* sws_ctx = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket packet;

    while (av_read_frame(fmt_ctx, &packet) >= 0) {
        if (packet.stream_index == video_index) {
            if (avcodec_send_packet(codec_ctx, &packet) == 0) {
                while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    if (!sws_ctx) {
                        sws_ctx = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                            frame->width, frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    }

                    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_BGRA, frame->width, frame->height, 1);
                    std::vector<uint8_t> temp_bgra(num_bytes);
                    uint8_t* pointers[4] = { temp_bgra.data(), nullptr, nullptr, nullptr };
                    int linesizes[4] = { frame->width * 4, 0, 0, 0 };
                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, pointers, linesizes);

                    VideoFrame vf;
                    vf.width = frame->width;
                    vf.height = frame->height;
                    vf.pts = frame->best_effort_timestamp * av_q2d(fmt_ctx->streams[video_index]->time_base);
                    vf.data.resize(num_bytes);

                    for (int y = 0; y < vf.height; y++) {
                        uint8_t* src = temp_bgra.data() + y * (vf.width * 4);
                        uint8_t* dst = vf.data.data() + y * (vf.width * 4);
                        for (int x = 0; x < vf.width; x++) {
                            uint8_t b = src[x * 4 + 0], g = src[x * 4 + 1], r = src[x * 4 + 2], a = src[x * 4 + 3];
                            dst[x * 4 + 0] = (uint8_t)((b * a + 127) / 255);
                            dst[x * 4 + 1] = (uint8_t)((g * a + 127) / 255);
                            dst[x * 4 + 2] = (uint8_t)((r * a + 127) / 255);
                            dst[x * 4 + 3] = a;
                        }
                    }

                    std::unique_lock<std::mutex> lock(queue_mtx);
                    cv.wait(lock, [] { return frame_queue.size() < MAX_QUEUE_SIZE; });
                    frame_queue.push(std::move(vf));
                    cv.notify_one();
                }
            }
        }
        av_packet_unref(&packet);
    }
    decoding_finished = true;
    cv.notify_all();
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
}
// --- メインスレッド (再生・表示) ---
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    SetupConsole();
    const char* movie_path = "tosikosi.webm";

    // デコード開始
    std::thread decoder(DecodeThread, movie_path);

    // 最初のフレームを待機（これで動画サイズを確定させる）
    VideoFrame first;
    {
        std::unique_lock<std::mutex> lock(queue_mtx);
        cv.wait(lock, [] { return !frame_queue.empty(); });
        first = frame_queue.front();
    }

    WNDCLASS wc = {};
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"FFmpegOverlay";
    RegisterClass(&wc);

    HWND hWnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
        wc.lpszClassName, L"Overlay", WS_POPUP,
        0, 0, first.width, first.height,
        nullptr, nullptr, hInstance, nullptr
    );

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    // --- 年越し待機ロジックの改善 ---
    auto get_target_time = []() {
        auto now = std::chrono::system_clock::now();
        std::time_t t_now = std::chrono::system_clock::to_time_t(now);

        std::tm t{};
        localtime_s(&t, &t_now);

        t.tm_mon = 11;   // 12月 (0始まり)
        t.tm_mday = 31;   // 31日
        t.tm_hour = 23;
        t.tm_min = 59;
        t.tm_sec = 54;

        auto target = std::chrono::system_clock::from_time_t(std::mktime(&t));

        // すでに過ぎてたら来年の12/31へ
        if (target <= now) {
            t.tm_year += 1;
            target = std::chrono::system_clock::from_time_t(std::mktime(&t));
        }

        return target;
        };


    auto target_time = get_target_time();
    MSG msg = { 0 };

    std::cout << "[INFO] Waiting for target time..." << std::endl;

    // 待機中もメッセージループを回す
    while (true) {
        auto now = std::chrono::system_clock::now();
        if (now >= target_time) break;

        // Windowsのメッセージを処理（フリーズ防止）
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // カウントダウン表示
        auto diff = std::chrono::duration_cast<std::chrono::seconds>(target_time - now).count();
        static long long last_diff = -1;
        if (diff != last_diff) {
            std::cout << "Starting in: " << diff << " seconds...   \r" << std::flush;
            last_diff = diff;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::cout << "\n[START] Playing video!" << std::endl;

    // 再生開始時間の記録
    auto start_time = std::chrono::high_resolution_clock::now();

    while (true) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        VideoFrame current;
        {
            std::unique_lock<std::mutex> lock(queue_mtx);

            // キューが空の場合の処理
            if (frame_queue.empty()) {
                if (decoding_finished) break; // 再生終了
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            auto now_high = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double>(now_high - start_time).count();

            // PTSチェック
            if (frame_queue.front().pts > elapsed) {
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            current = std::move(frame_queue.front());
            frame_queue.pop();
            cv.notify_one(); // デコーダーを起こす
        }

        // --- 描画処理 ---
        BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER), current.width, -current.height, 1, 32, BI_RGB } };
        void* bits = nullptr;
        HBITMAP hBmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (hBmp) {
            memcpy(bits, current.data.data(), current.data.size());
            HGDIOBJ oldBmp = SelectObject(hdcMem, hBmp);
            SIZE size = { current.width, current.height };
            POINT ptSrc = { 0, 0 };
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(hWnd, hdcScreen, nullptr, &size, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

            if (!IsWindowVisible(hWnd)) ShowWindow(hWnd, SW_SHOWNOACTIVATE);

            SelectObject(hdcMem, oldBmp);
            DeleteObject(hBmp);
        }
    }

    decoder.join();
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return 0;
}
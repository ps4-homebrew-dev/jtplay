// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playback through libmpv. mpv demuxes, decodes and plays the audio itself;
// the video is drawn with mpv's OpenGL renderer straight into the window's
// backbuffer, under the RmlUi document (see Backend::BeginFrame()). Any URL
// mpv opens plays, which covers everything a DLNA server serves and every
// plugin's stream (http, https, HLS, ...).
//
// Audio-only files get a spectrum from an ffmpeg filter in place of the
// picture ("lavfi-complex"); the visualizer turns it into the background
// the now-playing card is shown over.
#ifndef PLAYER_H
#define PLAYER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct mpv_handle;
struct mpv_render_context;
struct mpv_node;

// One selectable elementary stream of the open file.
struct TrackInfo {
  int stream_index = -1; // mpv track id, stable while the file is open
  std::string label;     // display string ("eng · ac3 5.1(side)", ...)
};

using AudioTrackInfo = TrackInfo; // "eng · ac3 5.1(side)"
using VideoTrackInfo = TrackInfo; // "h264 1920x1080"
using SubtitleTrackInfo = TrackInfo; // "eng · Forced · subrip"

// Threads:
//   mpv's own    demuxing, decoding, audio output
//   event thread mpv_wait_event(): file and property changes into the
//                members below
//   main thread  RenderVideo() and the small queries
//   app worker   Open() and Stop(), which block
class Player {
public:
  Player();
  ~Player();

  // Main thread, with the window's GL context current: creates the mpv core
  // and its GL renderer. Without it nothing plays, and Open() says so.
  // 'fonts_dir' is where subtitles find a font when the system has none
  // to offer (the console has no fontconfig).
  bool Initialize(std::string& error, const std::string& fonts_dir = std::string());
  // Main thread, GL context still current.
  void Shutdown();

  // Opens 'url' and starts playback at 'start_seconds', replacing whatever
  // is playing. Blocks until mpv has opened the file or given up on it;
  // call from a worker thread.
  bool Open(const std::string& url, std::string& error, int64_t start_seconds = 0);

  // Any thread: makes an Open() in progress give up at once (it returns
  // false with "cancelled"). Does nothing when no open is in progress.
  void AbortOpen();

  // Stops playback.
  void Stop();

  bool IsPlaying() const { return active_; }

  // True once the open file has played to its end (or failed mid-way). The
  // app uses it to leave the player or advance to the next item.
  bool AtEnd() const;

  // Main thread. Draws the current video frame into framebuffer 0 at
  // width x height; called after the backbuffer has been cleared and
  // before the UI is rendered over it. Draws nothing for audio-only files.
  void RenderVideo(int width, int height);

  // Audio-only files: the current spectrum frame, kSpectrumWidth x
  // kSpectrumHeight white bars on black (low frequencies left, log scaled
  // both ways: bar height spans -120..0 dB), rendered into framebuffer
  // 'fbo'. False when there is none.
  static constexpr int kSpectrumWidth = 128;
  static constexpr int kSpectrumHeight = 96;
  bool HasSpectrum() const { return vis_aid_ >= 0 && (active_ || opening_); }
  bool RenderSpectrum(unsigned fbo);

  // Short human-readable status ("h264 1920x1080 + aac", "Opening...", ...)
  // for display in the UI. Empty = nothing.
  std::string StatusText() const;

  // --- Transport ----------------------------------------------------------
  bool CanSeek() const { return seekable_; }
  // Position and duration in microseconds; -1 when unknown.
  int64_t PositionUs() const { return position_us_.load(); }
  int64_t DurationUs() const { return duration_us_.load(); }
  // How far ahead of the position the stream is buffered, as a position
  // in microseconds; -1 when unknown (local files report nothing useful).
  int64_t BufferedUs() const { return buffered_us_.load(); }
  // Seeks by a relative amount; stops a second short of the end.
  void SeekRelative(int64_t delta_us);
  void SetPaused(bool paused);
  // Returns the new paused state.
  bool TogglePause();
  bool IsPaused() const { return paused_; }
  // Saves the current video frame (no subtitles or OSD) to 'path'; the
  // format follows the extension. Blocks until written. False when there
  // is no video or mpv could not do it.
  bool Snapshot(const std::string& path);

  // Playback speed, 1.0 = normal. Reset to normal for every file.
  void SetSpeed(double speed);
  double Speed() const { return speed_.load(); }

  // --- Video tracks -------------------------------------------------------
  // The video streams of the open file, in file order (cover art excluded).
  std::vector<VideoTrackInfo> VideoTracks() const;
  int VideoTrackCount() const { return vtrack_count_.load(); }
  // mpv id of the track being shown, or -1.
  int CurrentVideoStream() const { return video_id_.load(); }
  // Label of the track being shown; "" when there is no video.
  std::string CurrentVideoLabel() const;
  // Switches to another video track of the open file. False if the id is
  // unknown or already active, or nothing is playing.
  bool SelectVideoTrack(int stream_index);

  // --- Audio tracks -------------------------------------------------------
  std::vector<AudioTrackInfo> AudioTracks() const;
  int AudioTrackCount() const { return atrack_count_.load(); }
  int CurrentAudioStream() const;
  std::string CurrentAudioLabel() const;
  bool SelectAudioTrack(int stream_index);

  // --- Subtitles ------------------------------------------------------------
  std::vector<SubtitleTrackInfo> SubtitleTracks() const;
  // mpv id of the subtitle shown, or -1 when they are off.
  int CurrentSubtitleStream() const { return sub_id_.load(); }
  // Shows track 'stream_index', or turns subtitles off for -1.
  bool SelectSubtitleTrack(int stream_index);

private:
  void EventThread();
  void UpdateTracks(const mpv_node* list);
  // Feeds audio track 'aid' to the spectrum filter, or (-1) turns it off.
  void SetVisualizer(int aid);
  void Command(const char* const* args);
  bool RenderTo(unsigned fbo, int width, int height);
  void ResetFileState();

  mpv_handle* mpv_ = nullptr;
  mpv_render_context* render_ = nullptr;
  std::thread event_thread_;
  std::atomic<bool> quitting_{false};

  // --- what the event thread has seen of files (guarded by mutex_) -------
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  int64_t started_count_ = 0;   // start-file events so far
  int64_t started_id_ = -1;     // playlist entry of the last start-file
  int64_t loaded_id_ = -1;      // ... of the last file-loaded
  int64_t ended_id_ = -1;       // ... of the last end-file
  std::string end_error_;       // why that one ended, if it failed
  std::string last_log_error_;  // mpv's last error message

  // --- the open file --------------------------------------------------------
  std::atomic<bool> active_{false};
  std::atomic<bool> opening_{false};
  bool abort_open_ = false;             // guarded by mutex_
  std::atomic<int64_t> playing_id_{-1}; // playlist entry being played
  std::atomic<bool> seekable_{false};
  std::atomic<bool> paused_{false};
  std::atomic<int64_t> position_us_{-1};
  std::atomic<int64_t> duration_us_{-1};
  std::atomic<int> video_id_{-1};
  std::atomic<int> audio_id_{-1};
  std::atomic<int> sub_id_{-1};
  std::atomic<int64_t> buffered_us_{-1};
  std::atomic<double> speed_{1.0};
  std::atomic<int> vis_aid_{-1}; // audio track feeding the spectrum, or -1

  mutable std::mutex tracks_mutex_;
  std::vector<VideoTrackInfo> video_tracks_;
  std::vector<AudioTrackInfo> audio_tracks_;
  std::vector<SubtitleTrackInfo> sub_tracks_;
  std::string track_status_;
  std::atomic<int> vtrack_count_{0};
  std::atomic<int> atrack_count_{0};
};

#endif

// Headless check of the real-time host: plays a working file on a null audio device and reports
// underruns and render load; exercises seek, live parameter changes and A/B.
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <thread>

#include "rt/host.h"

using namespace sermon;
using namespace sermon::rt;

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: host_test work.wav [seconds]\n"); return 2; }
  const double secs = argc > 2 ? std::atof(argv[2]) : 8;
  WavFileSource src(argv[1]);
  std::shared_ptr<const FileModel> model = buildFileModel(src, Tone::Tone1);
  Host host;
  std::string err;
  if (!host.open(argv[1], model, true, 4096, &err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return 1; }
  ChainParams p;
  host.setParams(p);
  host.play();
  auto wait = [](double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); };
  wait(secs / 4);
  auto st = host.state();
  std::printf("t=%.2f pos=%.2f load=%.3f underruns=%llu lufs(st)=%.1f\n", secs / 4, st.position, st.renderLoad, (unsigned long long)st.underruns, st.shortTermLufs);
  host.seek(host.duration() * 0.5);
  wait(secs / 4);
  p.deess = Intensity::More; host.setParams(p); host.setOriginal(true);
  wait(secs / 4);
  host.setOriginal(false);
  wait(secs / 4);
  st = host.state();
  std::printf("end pos=%.2f load=%.3f underruns=%llu peak=%.3f/%.3f\n", st.position, st.renderLoad, (unsigned long long)st.underruns, st.peakL, st.peakR);
  host.close();
  return st.underruns == 0 ? 0 : 1;
}

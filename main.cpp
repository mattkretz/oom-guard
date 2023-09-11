/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright © 2023 GSI Helmholtzzentrum fuer Schwerionenforschung GmbH
 *                  Matthias Kretz <m.kretz@gsi.de>
 */

#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <experimental/simd>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <new>
#include <optional>
#include <thread>

namespace stdx = std::experimental;
namespace fs = std::filesystem;

template <typename T, int N>
using simd = stdx::simd<T, stdx::simd_abi::deduce_t<T, N>>;

template <typename T, int N>
  constexpr simd<T, N> dec_digits([](auto i) {
              T r = 1;
              for (int j = N - 1 - i; j; --j)
                r *= 10;
              return r;
            });

/*template <typename T, typename Abi>
  void print(const stdx::simd<T, Abi>& v)
  {
    std::cout << '[' << v[0];
    for (int i = 1; i < v.size(); ++i)
      std::cout << ", " << v[i];
    std::cout << ']';
  }*/

std::array<std::byte, 2 * 1024 * 1024> s_buffer0;
std::array<std::byte, 512 * 1024> s_buffer1;
std::pmr::monotonic_buffer_resource s_outer_memory {s_buffer0.data(), s_buffer0.size()};
std::pmr::monotonic_buffer_resource s_inner_memory {s_buffer1.data(), s_buffer1.size()};

std::pmr::monotonic_buffer_resource* s_memory_ptr = &s_outer_memory;

void*
operator new(std::size_t n)
{
  void* ptr = s_memory_ptr->allocate(n);
  //std::fprintf(stderr, "new(%zd) -> %p\n", n, ptr);
  return ptr;
}

void
operator delete(void*) {}

void
operator delete[](void*) {}

void
operator delete(void*, std::size_t) {}

void
operator delete[](void*, std::size_t) {}

std::optional<unsigned short>
string_to_ushort(const char* s, const int size) noexcept
{
  assert(size <= 8);
  if (size <= 0)
    return 0;

  alignas(16) char name[9] = "00000000";
  std::memcpy(name + 8 - size, s, size);
  simd<char, 8> c(name, stdx::overaligned<16>);
  where(c == ' ', c) = '0';
  if (any_of(c < '0' or c > '9'))
    return std::nullopt;

  return reduce(stdx::static_simd_cast<simd<unsigned short, 8>>(c - '0')
                  * dec_digits<unsigned short, 8>);
}

class inner_memory_scope
{
public:
  inner_memory_scope()
  {
    s_inner_memory.release();
    s_memory_ptr = &s_inner_memory;
  }

  ~inner_memory_scope()
  { s_memory_ptr = &s_outer_memory; }
};

int
main()
{
  std::printf("oom-guard 0.2\n"
              "Copyright © 2023 GSI Helmholtzzentrum fuer Schwerionenforschung GmbH\n"
              "License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>.\n"
              "This is free software: you are free to change and redistribute it.\n"
              "There is NO WARRANTY, to the extent permitted by law.\n\n"
              "Written by Matthias Kretz.\n");
  {
    int status = mlock(s_buffer0.data(), s_buffer0.size());
    status += mlock(s_buffer1.data(), s_buffer1.size());
    status += mlockall(MCL_CURRENT);
    if (status != 0)
      std::perror("mlock");
  }
  for (;;)
    {
      s_outer_memory.release();

      using namespace std::chrono_literals;
      auto sleep_time = 1000ms;

      std::ifstream meminfo("/proc/meminfo");
      meminfo.seekg(4 * 16 + 8);
      alignas(16) char buf[16] = {};
      meminfo.read(buf, 8);
      simd<char, 8> characters(buf, stdx::overaligned<8>);
      where(characters == ' ', characters) = '0';
      const auto digits = std::bit_cast<simd<short, 5>>(
                            stdx::simd_cast<simd<short, 8>>(characters - '0'));
      const int avail = reduce(digits * dec_digits<short, 5>);

      const fs::path proc("/proc");
      if (avail < 750)
        {
          int max = 0;
          pid_t pid = -1;
          char cmdline_path[128];
          for(const auto& dir : fs::directory_iterator(proc))
            {
              inner_memory_scope _;
              if (not dir.is_directory())
                continue;
              const fs::directory_entry oom_score_adj(dir.path() / "oom_score_adj");
              if (not oom_score_adj.exists())
                continue;
              std::ifstream file(oom_score_adj.path());
              file.read(buf, 8);
              if (buf[0] == '-' or buf[0] == '0')
                continue;
              const auto oom_value = string_to_ushort(buf, file.gcount() - 1);
              if (not oom_value)
                continue;
              const int value = oom_value.value();
              if (value > 200) // kill unconditionally
                {
                  const std::string& filename = dir.path().filename().string();
                  if (filename.size() > 8)
                    continue;
                  const auto pid_value = string_to_ushort(filename.c_str(), filename.size());
                  if (not pid_value)
                    continue;
                  kill(pid_value.value(), SIGTERM);
                  char cmdline[256];
                  std::ifstream(dir.path() / "cmdline") >> cmdline;
                  std::printf("Terminating PID %d (score %d): %s\n", pid, max, cmdline);
                }
              else if (value > max)
                {
                  const std::string& filename = dir.path().filename().string();
                  if (filename.size() > 8)
                    continue;
                  const auto pid_value = string_to_ushort(filename.c_str(), filename.size());
                  if (not pid_value)
                    continue;
                  max = value;
                  pid = pid_value.value();
                  const auto cmdline_string = (dir.path() / "cmdline").string();
                  if (cmdline_string.size() >= sizeof(cmdline_path))
                    continue;
                  std::strcpy(cmdline_path, cmdline_string.c_str());
                }
            }
          if (max > 0)
            {
              kill(pid, SIGTERM);
              char cmdline[256];
              std::ifstream(cmdline_path) >> cmdline;
              std::printf("Terminating PID %d (score %d): %s\n", pid, max, cmdline);
              //kill(pid, SIGKILL);
            }
          else
            std::printf("Found nothing to kill\n");
          system("/usr/bin/akonadictl stop");

          // reduce sleep time, a process might be in the process of allocating the remaining memory
          sleep_time = 5ms;
        }
      //else
        //std::printf("All good: %d MiB available\n", avail);

      std::this_thread::sleep_for(sleep_time);
    }
}

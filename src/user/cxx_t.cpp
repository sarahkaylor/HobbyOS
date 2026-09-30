/*
 * CXX_T.BIN — libc++ acceptance test (browser.md §6 P3).
 *
 * A crt0-style program (main()) compiled as freestanding C++23 against the
 * vendored libc++ headers and linked against obj/<arch>/libcxx.a +
 * libc.a.  It exercises the standard-library surface the milestone claims:
 *
 *   <vector> <string> <map> <unordered_map> <algorithm> <memory> <atomic>
 *   <thread> <mutex> <condition_variable> <chrono> <sstream>
 *
 * including the libc dependencies those pull in: the pthread surface
 * (create/join, mutex, condvar wait + timedwait), clock_gettime +
 * nanosleep via <chrono>/<thread>, the C-locale facet layer (num_get/
 * num_put over strtoll_l/strtold_l/...), std::to_string (to_chars),
 * operator new/delete over the HobbyOS allocator.
 *
 * Output convention (wave-scannable): "  CXX_T <name> : PASS" per check,
 * then "ALL TESTS PASSED SUCCESSFULLY!" — or a FAIL line naming the check.
 * The process exits with the failure count.
 */

#include "libc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static int checks;
static int fails;

static void check(const char *name, int ok) {
  checks++;
  print_console("  CXX_T ");
  print_console(name);
  print_console(ok ? " : PASS\n" : " : FAIL\n");
  if (!ok)
    fails++;
}

/* Runtime seed: volatile so -O2 cannot constant-fold the whole test. */
static volatile int seed = 11;
static volatile int zero;

/* ---------------- shared thread fixtures ---------------- */

static std::atomic<long> g_atomic;
static long g_guarded;
static std::mutex g_mu;
static std::condition_variable g_cv;
static bool g_go;
static int g_started;

static void atomic_worker() {
  for (int i = 0; i < 500; i++)
    g_atomic.fetch_add(1, std::memory_order_relaxed);
}

static void locked_worker() {
  for (int i = 0; i < 500; i++) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_guarded++;
  }
}

static void cv_worker() {
  std::unique_lock<std::mutex> lk(g_mu);
  g_started++;
  g_cv.notify_one();
  g_cv.wait(lk, [] { return g_go; });
  g_guarded += 1000;
}

static void delayed_notifier() {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_go = true;
  }
  g_cv.notify_all();
}

/* crt0 calls the plain C symbol `main` (see src/libc/crt0.c); the other
 * C++ tests wrap main the same way (src/user/cxx_smoke.cpp). */
extern "C" int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  const int s = seed; /* runtime value */

  /* ---------------- <vector> ---------------- */
  {
    std::vector<int> v;
    for (int i = 0; i < 64; i++)
      v.push_back((i * s + 3) % 47);
    const std::size_t n0 = v.size();
    v.push_back(-1);
    v.pop_back();
    bool grew = v.size() == n0 && v.capacity() >= n0;
    int sum = 0;
    for (int x : v)
      sum += x;
    int sum2 = 0;
    for (std::size_t i = 0; i < v.size(); i++)
      sum2 += v[i];
    v.erase(v.begin() + 2);
    bool erased = v.size() == n0 - 1;
    check("vector_ops", grew && sum == sum2 && erased && v.front() ==
                                                              (s * 0 + 3) % 47);
  }

  /* vector<string>: heap strings + moves through reallocation + sort */
  {
    std::vector<std::string> names;
    const char *words[] = {"delta", "alpha", "charlie", "bravo", "echo"};
    for (int i = 0; i < 5; i++)
      names.push_back(words[(i * s) % 5]);
    std::sort(names.begin(), names.end());
    bool sorted = std::is_sorted(names.begin(), names.end());
    bool kept = names.size() == 5 && names.front().size() > 0;
    names.insert(names.begin(), std::string("aaa"));
    names.resize(7, std::string("zzz"));
    check("vector_string", sorted && kept && names.size() == 7 &&
                               names[6] == "zzz");
  }

  /* ---------------- <string> ---------------- */
  {
    std::string a = "hello";
    std::string b = "world";
    a += ", ";
    a += b;
    std::string c = a + "!";
    std::size_t pos = c.find("world");
    std::string sub = c.substr(0, 5);
    bool ok = c.size() == 13 && pos == 7 && sub == "hello" &&
              c.compare(7, 5, b) == 0 && c[0] == 'h' && c.rfind('!') == 12;
    check("string_ops", ok);
  }

  {
    std::string n = std::to_string(s * 1000 + 7);
    std::string l = std::to_string(-(long)s);
    bool ok = n == "11007" && l == "-11" && std::to_string(0) == "0";
    check("string_to_string", ok);
  }

  /* ---------------- <map> (ordered) ---------------- */
  {
    std::map<std::string, int> m;
    for (int i = 0; i < 20; i++)
      m[std::string(1, (char)('a' + (i * s) % 20))] += i;
    bool ordered = true;
    std::string prev;
    for (const auto &kv : m) {
      if (!prev.empty() && !(prev < kv.first))
        ordered = false;
      prev = kv.first;
    }
    std::size_t n = m.size();
    m.erase(m.begin());
    m["zz"] = 5;
    bool ok = ordered && n > 0 && m.size() == n && m.count("zz") == 1 &&
              m.find("zz")->second == 5 && m.count("no-such-key") == 0;
    check("map_ops", ok);
  }

  /* ---------------- <unordered_map> ---------------- */
  {
    std::unordered_map<std::string, int> u;
    for (int i = 0; i < 200; i++)
      u["key" + std::to_string(i)] = i;
    bool count_ok = u.size() == 200;
    bool find_ok = u.find("key199") != u.end() && u["key199"] == 199;
    u.erase("key0");
    bool erase_ok = u.size() == 199 && u.count("key0") == 0;
    int seen = 0;
    for (const auto &kv : u)
      seen += (kv.second >= 0);
    u.rehash(512);
    check("unordered_map_ops",
          count_ok && find_ok && erase_ok && seen == 199 &&
              u.bucket_count() >= 200);
  }

  /* ---------------- <algorithm> ---------------- */
  {
    std::vector<int> v;
    for (int i = 0; i < 97; i++)
      v.push_back((i * 37 + s) % 89);
    std::sort(v.begin(), v.end());
    bool sorted = std::is_sorted(v.begin(), v.end());
    auto it = std::find(v.begin(), v.end(), 42);
    bool found = it != v.end() && *it == 42;
    int cnt = (int)std::count(v.begin(), v.end(), 42);
    int expected = 0;
    for (int x : v)
      expected += (x == 42);
    check("algorithm_sort_find", sorted && found && cnt == expected && cnt > 0);
  }

  {
    std::vector<int> v;
    for (int i = 0; i < 50; i++)
      v.push_back(i % 2 ? i : -i);
    bool all_odd_pos = std::all_of(v.begin(), v.end(), [](int x) {
      return x == 0 || (x > 0) == (x % 2 != 0);
    });
    int neg = (int)std::count_if(v.begin(), v.end(), [](int x) { return x < 0; });
    bool any_big = std::any_of(v.begin(), v.end(), [](int x) { return x > 40; });
    bool none_big = std::none_of(v.begin(), v.end(), [](int x) { return x > 100; });
    bool minmax = *std::min_element(v.begin(), v.end()) == -48 &&
                  *std::max_element(v.begin(), v.end()) == 49;
    check("algorithm_predicates",
          all_odd_pos && neg == 24 && any_big && none_big && minmax);
  }

  /* ---------------- <memory> ---------------- */
  {
    std::unique_ptr<int> p(new int(41));
    *p += 1;
    std::unique_ptr<int[]> arr(new int[4]);
    for (int i = 0; i < 4; i++)
      arr[i] = i * s;
    auto q = std::make_unique<std::string>("unique");
    std::unique_ptr<int> moved = std::move(p);
    bool ok = *moved == 42 && p == nullptr && q->size() == 6 &&
              arr[3] == 3 * s;
    check("memory_unique_ptr", ok);
  }

  {
    std::shared_ptr<std::string> a = std::make_shared<std::string>("shared");
    std::shared_ptr<std::string> b = a;
    std::weak_ptr<std::string> w = a;
    bool counts = a.use_count() == 2 && !w.expired();
    b.reset();
    std::shared_ptr<std::string> c = w.lock();
    bool revived = c && *c == "shared" && a.use_count() == 2;
    c.reset();
    a.reset();
    bool expired = w.expired();
    check("memory_shared_ptr", counts && revived && expired);
  }

  /* ---------------- <atomic> ---------------- */
  {
    std::atomic<int> a{zero};
    for (int i = 0; i < 100; i++)
      a.fetch_add(1, std::memory_order_seq_cst);
    int expected = 100;
    bool cas = a.compare_exchange_strong(expected, 7);
    bool loaded = a.load() == 7 && cas;
    std::atomic<long> l{0};
    l.store(123);
    check("atomic_counter", a.load() == 7 && loaded && l.exchange(5) == 123);
  }

  /* ---------------- <thread> + <atomic> ---------------- */
  {
    g_atomic.store(0);
    std::thread t1(atomic_worker);
    std::thread t2(atomic_worker);
    t1.join();
    t2.join();
    check("threads_atomic", g_atomic.load() == 1000);
  }

  /* ---------------- <thread> + <mutex> ---------------- */
  {
    g_guarded = 0;
    std::thread t1(locked_worker);
    std::thread t2(locked_worker);
    std::thread t3(locked_worker);
    t1.join();
    t2.join();
    t3.join();
    check("threads_mutex", g_guarded == 1500);
  }

  /* ---------------- <condition_variable> ---------------- */
  {
    g_guarded = 0;
    g_started = 0;
    g_go = false;
    std::thread t(cv_worker);
    {
      std::unique_lock<std::mutex> lk(g_mu);
      bool notified = g_cv.wait_for(lk, std::chrono::milliseconds(1000),
                                    [] { return g_started > 0; });
      check("condvar_wait_notify", notified && g_started == 1);
    }
    std::thread notifier(delayed_notifier);
    notifier.join();
    t.join();
    check("condvar_wakeup",
          g_go && g_guarded == 1000); /* worker ran to completion */
  }

  /* condvar timedwait: a wait_for with a predicate that never becomes
   * true must time out (pthread_cond_timedwait -> ETIMEDOUT). */
  {
    std::unique_lock<std::mutex> lk(g_mu);
    auto t0 = std::chrono::steady_clock::now();
    bool ok = !g_cv.wait_for(lk, std::chrono::milliseconds(40),
                             [] { return false; });
    auto t1 = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        t1 - t0).count();
    check("condvar_timedwait", ok && elapsed >= 30);
  }

  /* ---------------- <chrono> ---------------- */
  {
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    bool slept = ms.count() >= 20;
    bool ordered = t1 > t0;
    std::chrono::nanoseconds ns = std::chrono::milliseconds(2) + 
                                  std::chrono::microseconds(500);
    bool conv = std::chrono::duration_cast<std::chrono::microseconds>(ns)
                    .count() == 2500;
    check("chrono_sleep", slept && ordered && conv);
  }

  {
    /* steady stamp: non-negative, advances across a sleep, and the epoch
     * converts cleanly. */
    auto a = std::chrono::steady_clock::now().time_since_epoch();
    auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(a);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto b = std::chrono::steady_clock::now().time_since_epoch();
    auto stamp2 = std::chrono::duration_cast<std::chrono::milliseconds>(b);
    check("chrono_stamp", stamp.count() >= 0 && stamp2 > stamp &&
                              stamp2.count() - stamp.count() >= 4);
  }

  /* ---------------- <sstream> ---------------- */
  {
    std::ostringstream os;
    os << "value=" << (s * 3) << " str=" << "ok" << " hex=" << std::hex << 255
       << std::dec << " end";
    std::string out = os.str();
    bool ok = out == "value=33 str=ok hex=ff end";
    std::ostringstream os2;
    os2 << std::setw(8) << std::setfill('0') << 123;
    check("sstream_format", ok && os2.str() == "00000123");
  }

  {
    std::istringstream is("17 42 hello 3.5");
    int a = 0, b = 0;
    std::string word;
    double d = 0;
    is >> a >> b >> word >> d;
    bool ok = a == 17 && b == 42 && word == "hello" && !is.fail() &&
              d > 3.49 && d < 3.51;
    check("sstream_parse", ok);
  }

  if (fails == 0)
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
  else
    print_console("CXX_T FAILURES\n");
  exit(fails);
}

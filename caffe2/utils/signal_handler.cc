#include "caffe2/utils/signal_handler.h"
#include "caffe2/core/logging.h"

#if defined(_MSC_VER)

// TODO: Currently we do not support signal handling in Windows yet - below is
// a minimal implementation that makes things compile.
namespace caffe2 {
SignalHandler::SignalHandler(
    SignalHandler::Action SIGINT_action,
    SignalHandler::Action SIGHUP_action) {}
SignalHandler::~SignalHandler() {}
bool SignalHandler::GotSIGINT() {
  return false;
}
bool SignalHandler::GotSIGHUP() {
  return false;
}
SignalHandler::Action SignalHandler::CheckForSignals() {
  return SignalHandler::Action::NONE;
}
} // namespace caffe2

#else // defined(_MSC_VER)

// Normal signal handler implementation.

#include <cxxabi.h>
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include "caffe2/core/init.h"

namespace {

struct sigaction previousSighup;
struct sigaction previousSigint;
std::atomic<int> sigintCount(0);
std::atomic<int> sighupCount(0);
std::atomic<int> hookedUpCount(0);

void handleSignal(int signal) {
  switch (signal) {
    // TODO: what if the previous handler uses sa_sigaction?
    case SIGHUP:
      sighupCount += 1;
      if (previousSighup.sa_handler) {
        previousSighup.sa_handler(signal);
      }
      break;
    case SIGINT:
      sigintCount += 1;
      if (previousSigint.sa_handler) {
        previousSigint.sa_handler(signal);
      }
      break;
  }
}

void hookupHandler() {
  if (hookedUpCount++) {
    return;
  }
  struct sigaction sa;
  // Setup the handler
  sa.sa_handler = &handleSignal;
  // Restart the system call, if at all possible
  sa.sa_flags = SA_RESTART;
  // Block every signal during the handler
  sigfillset(&sa.sa_mask);
  // Intercept SIGHUP and SIGINT
  if (sigaction(SIGHUP, &sa, &previousSighup) == -1) {
    LOG(FATAL) << "Cannot install SIGHUP handler.";
  }
  if (sigaction(SIGINT, &sa, &previousSigint) == -1) {
    LOG(FATAL) << "Cannot install SIGINT handler.";
  }
}

// Set the signal handlers to the default.
void unhookHandler() {
  if (--hookedUpCount > 0) {
    return;
  }
  struct sigaction sa;
  // Setup the sighub handler
  sa.sa_handler = SIG_DFL;
  // Restart the system call, if at all possible
  sa.sa_flags = SA_RESTART;
  // Block every signal during the handler
  sigfillset(&sa.sa_mask);
  // Intercept SIGHUP and SIGINT
  if (sigaction(SIGHUP, &previousSighup, nullptr) == -1) {
    LOG(FATAL) << "Cannot uninstall SIGHUP handler.";
  }
  if (sigaction(SIGINT, &previousSigint, nullptr) == -1) {
    LOG(FATAL) << "Cannot uninstall SIGINT handler.";
  }
}

#if defined(__linux__)
// We need to hold a reference to call the previous SIGUSR2 handler in case
// we didn't signal it
struct sigaction previousSigusr2;
// Flag dictating whether the SIGUSR2 handler falls back to previous handlers
// or is intercepted in order to print a stack trace.
std::atomic<bool> fatalSignalReceived(false);
// Global state set when a fatal signal is received so that backtracing threads
// know why they're printing a stacktrace.
const char* fatalSignalName("<UNKNOWN>");
int fatalSignum(-1);
// This wait condition is used to wait for other threads to finish writing
// their stack trace when in fatal sig handler (we can't use pthread_join
// because there's no way to convert from a tid to a pthread_t).
pthread_cond_t writingCond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t writingMutex = PTHREAD_MUTEX_INITIALIZER;

struct {
  const char* name;
  int signum;
  struct sigaction previous;
} kSignalHandlers[] = {
  { "SIGABRT",  SIGABRT,  {} },
  { "SIGINT",   SIGINT,   {} },
  { "SIGILL",   SIGILL,   {} },
  { "SIGFPE",   SIGFPE,   {} },
  { "SIGBUS",   SIGBUS,   {} },
  { "SIGSEGV",  SIGSEGV,  {} },
  { nullptr,    0,        {} }
};


struct sigaction* getPreviousSigaction(int signum) {
  for (auto handler = kSignalHandlers; handler->name != nullptr; handler++) {
    if (handler->signum == signum) {
      return &handler->previous;
    }
  }
  return nullptr;
}

const char* getSignalName(int signum) {
  for (auto handler = kSignalHandlers; handler->name != nullptr; handler++) {
    if (handler->signum == signum) {
      return handler->name;
    }
  }
  return nullptr;
}

void printStacktrace() {
  constexpr const unsigned maxFrames = 256;
  std::array<void*, maxFrames> frames;
  Dl_info info;
  int numberOfFrames = backtrace(frames.data(), maxFrames);
  for (int i = 0; i < numberOfFrames; i++) {
    const void* pc = frames[i];
    const char* path = nullptr;
    const char* name = "???";
    char* demangled = nullptr;
    int offset = -1;

    std::cerr << "[" << i << "] ";
    if (dladdr(pc, &info)) {
      path = info.dli_fname;
      name = info.dli_sname ?: "???";
      offset = reinterpret_cast<uintptr_t>(pc) -
          reinterpret_cast<uintptr_t>(info.dli_saddr);

      int status;
      demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
      if (status == 0) {
        name = demangled;
      }
    }
    std::cerr << name;
    if (offset >= 0) {
      std::cerr << "+" << reinterpret_cast<void*>(offset);
    }
    std::cerr << "(" << pc << ")";
    if (path) {
      std::cerr << " in " << path;
    }
    std::cerr << std::endl;
    if (demangled) {
      free(demangled);
    }
  }
}

void callPreviousSignalHandler(
    struct sigaction* action,
    int signum,
    siginfo_t* info,
    void* ctx) {
  if (!action->sa_handler) {
    return;
  }
  if ((action->sa_flags & SA_SIGINFO) == SA_SIGINFO) {
    action->sa_sigaction(signum, info, ctx);
  } else {
    action->sa_handler(signum);
  }
}

// needsLock signals whether we need to lock our writing mutex.
void stacktraceSignalHandler(bool needsLock) {
  if (needsLock) {
    pthread_mutex_lock(&writingMutex);
  }
  pid_t tid = syscall(SYS_gettid);
  std::cerr << fatalSignalName << "(" << fatalSignum << "), Thread " << tid
            << ": " << std::endl;
  printStacktrace();
  std::cerr << std::endl;
  if (needsLock) {
    pthread_mutex_unlock(&writingMutex);
    pthread_cond_signal(&writingCond);
  }
}

// Our fatal signal entry point
void fatalSignalHandler(int signum) {
  // Check if this is a proper signal that we declared above.
  const char* name = getSignalName(signum);
  if (!name) {
    return;
  }
  if (fatalSignalReceived) {
    return;
  }
  // Set the flag so that our SIGUSR2 handler knows that we're aborting and
  // that it should intercept any SIGUSR2 signal.
  fatalSignalReceived = true;
  // Set state for other threads.
  fatalSignum = signum;
  fatalSignalName = name;
  // Linux doesn't have a nice userland API for enumerating threads so we
  // need to use the proc pseudo-filesystem.
  DIR* procDir = opendir("/proc/self/task");
  if (procDir) {
    pid_t pid = getpid();
    pid_t currentTid = syscall(SYS_gettid);
    struct dirent* entry;
    pthread_mutex_lock(&writingMutex);
    while ((entry = readdir(procDir)) != nullptr) {
      if (entry->d_name[0] == '.') {
        continue;
      }
      pid_t tid = atoi(entry->d_name);
      // If we've found the current thread then we'll jump into the SIGUSR2
      // handler before calling pthread_cond_wait thus deadlocking, so branch
      // our directly to the backtrace handler instead of signaling it.
      if (tid != currentTid) {
        syscall(SYS_tgkill, pid, tid, SIGUSR2);
        pthread_cond_wait(&writingCond, &writingMutex);
      } else {
        stacktraceSignalHandler(false);
      }
    }
    pthread_mutex_unlock(&writingMutex);
  } else {
    perror("Failed to open /proc/self/task");
  }
  sigaction(signum, getPreviousSigaction(signum), nullptr);
  raise(signum);
}

// Our SIGUSR2 entry point
void stacktraceSignalHandler(int signum, siginfo_t* info, void* ctx) {
  if (fatalSignalReceived) {
    stacktraceSignalHandler(true);
  } else {
    // We don't want to actually change the signal handler as we want to
    // remain the signal handler so that we may get the usr2 signal later.
    callPreviousSignalHandler(&previousSigusr2, signum, info, ctx);
  }
}

#endif // defined(__linux__)

} // namespace

namespace caffe2 {

SignalHandler::SignalHandler(
    SignalHandler::Action SIGINT_action,
    SignalHandler::Action SIGHUP_action)
    : SIGINT_action_(SIGINT_action),
      SIGHUP_action_(SIGHUP_action),
      my_sigint_count_(sigintCount),
      my_sighup_count_(sighupCount) {
  hookupHandler();
}

SignalHandler::~SignalHandler() {
  unhookHandler();
}

// Return true iff a SIGINT has been received since the last time this
// function was called.
bool SignalHandler::GotSIGINT() {
  uint64_t count = sigintCount;
  bool result = (count != my_sigint_count_);
  my_sigint_count_ = count;
  return result;
}

// Return true iff a SIGHUP has been received since the last time this
// function was called.
bool SignalHandler::GotSIGHUP() {
  uint64_t count = sighupCount;
  bool result = (count != my_sighup_count_);
  my_sighup_count_ = count;
  return result;
}


SignalHandler::Action SignalHandler::CheckForSignals() {
  if (GotSIGHUP()) {
    return SIGHUP_action_;
  }
  if (GotSIGINT()) {
    return SIGINT_action_;
  }
  return SignalHandler::Action::NONE;
}

#if defined(__linux__)

namespace internal {

// Installs SIGABRT signal handler so that we get stack traces
// from every thread on SIGABRT caused exit. Also installs SIGUSR2 handler
// so that threads can communicate with each other (be sure if you use SIGUSR2)
// to install your handler before initing caffe2 (we properly fall back to
// the previous handler if we didn't initiate the SIGUSR2).
bool Caffe2InitFatalSignalHandler(int*, char***) {
  struct sigaction sa;
  sigemptyset(&sa.sa_mask);
  // Since we'll be in an exiting situation it's possible there's memory
  // corruption, so make our own stack just in case.
  sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
  sa.sa_handler = ::fatalSignalHandler;
  for (auto* handler = kSignalHandlers; handler->name != nullptr; handler++) {
    if (sigaction(handler->signum, &sa, &handler->previous)) {
      std::string str("Failed to add ");
      str += handler->name;
      str += " handler!";
      perror(str.c_str());
      return false;
    }
  }
  sa.sa_sigaction = ::stacktraceSignalHandler;
  if (sigaction(SIGUSR2, &sa, &::previousSigusr2)) {
    perror("Failed to add SIGUSR2 handler");
    return false;
  }
  return true;
}

REGISTER_CAFFE2_EARLY_INIT_FUNCTION(
    Caffe2InitFatalSignalHandler,
    &Caffe2InitFatalSignalHandler,
    "Inits signal handlers for fatal signals so we can see what"
    " went wrong");

} // namepsace internal

#endif // defined(__linux__)

}  // namespace caffe2

#endif // !defined(_MSC_VER)

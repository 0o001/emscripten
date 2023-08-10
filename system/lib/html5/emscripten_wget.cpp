#include <emscripten.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <unordered_map>

// Creates all ancestor directories of a given file, if they do not already
// exist. Returns 0 on success or 1 on error.
static int mkdirs(const char* file) {
  char* copy = strdup(file);
  char* c = copy;
  while (*c) {
    // Create any non-trivial (not the root "/") directory.
    if (*c == '/' && c != copy) {
      *c = 0;
      int result = mkdir(copy, S_IRWXU);
      *c = '/';
      // Continue while we succeed in creating directories or while we see that
      // they already exist.
      if (result < 0 && errno != EEXIST) {
        free(copy);
        return 1;
      }
    }
    c++;
  }
  free(copy);
  return 0;
}

int emscripten_wget(const char* url, const char* file) {
  // Create the ancestor directories.
  if (mkdirs(file)) {
    return 1;
  }

  // Fetch the data.
  void* buffer;
  int num;
  int error;
  emscripten_wget_data(url, &buffer, &num, &error);
  if (error) {
    return 1;
  }

  // Write the data.
  int fd = open(file, O_WRONLY | O_CREAT, S_IRWXU);
  if (fd >= 0) {
    write(fd, buffer, num);
    close(fd);
  }
  free(buffer);
  return fd < 0;
}

// Async wget

struct AsyncWgetData {
  const char* file;
  em_str_callback_func onload;
  em_str_callback_func onerror;
};

// Maps filenames to AsyncWgetData for all in-flight operations. This is needed
// because we can't pass an AsyncWgetData* through all the callbacks - the ones
// for preloading only have the filename.
std::unordered_map<std::string, AsyncWgetData*> _file_data_map;

static void _add_data_to_map(AsyncWgetData* data) {
  _file_data_map[data->file] = data;
}

static AsyncWgetData* _get_data_from_map_and_remove(const char* file) {
  auto iter = _file_data_map.find(file);
  if (iter == _file_data_map.end()) {
    return nullptr;
  }
  auto ret = iter->second;
  _file_data_map.erase(iter);
  return ret;
}

static void _wget_onload_onload(const char* file) {
  auto* data = _get_data_from_map_and_remove(file);
  data->onload(data->file);
  delete data;
}

static void _wget_onload_onerror(const char* file) {
  auto* data = _get_data_from_map_and_remove(file);
  data->onerror(data->file);
  delete data;
}

static void _wget_onload(void* arg, void* buf, int bufsize) {
  AsyncWgetData* data = (AsyncWgetData*)arg;

  // Write the file data.
  int fd = open(data->file, O_WRONLY | O_CREAT, S_IRWXU);
  if (fd < 0) {
    delete data;
    return;
  }
  write(fd, buf, bufsize);
  close(fd);

  // Add the data to the map so that we can access it in the callbacks we are
  // about to prepare for. Those callbacks only receive the string name, so the
  // map is needed.
  _add_data_to_map(data);

  // Perform preload operations. Only in those callbacks is the data freed.
  emscripten_run_preload_plugins(data->file,
                                 _wget_onload_onload,
                                 _wget_onload_onerror);
}

static void _wget_onerror(void* arg) {
  AsyncWgetData* data = (AsyncWgetData*)arg;
  data->onerror(data->file);
  free(data);
}

void emscripten_async_wget(const char* url, const char* file, em_str_callback_func onload, em_str_callback_func onerror) {
  // Create the ancestor directories.
  if (mkdirs(file)) {
    onerror(file);
    return;
  }

  // Allocate data, which will be freed in the async callbacks.
  auto* data = new AsyncWgetData{file, onload, onerror};
  emscripten_async_wget_data(url,
                             data,
                             _wget_onload,
                             _wget_onerror);
}

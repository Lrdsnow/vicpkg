#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define VICPKG_DIR "/data/vicpkg"
#define VERSIONS_DIR VICPKG_DIR "/versions"
#define FILES_DIR VICPKG_DIR "/files"
#define CACHE_DIR VICPKG_DIR "/cache"
#define LEGACY_INSTALL_DIR VICPKG_DIR "/legacy/installed"
#define BIN_DIR VICPKG_DIR "/bin"
#define REPOS_FILE VICPKG_DIR "/repos.list"
#define VICPKG_VERSION "1.0.0"
#define MAX_REPOS 10
#define MAX_PATH 512
#define MAX_LINE 2048
#define INSTALL_ROOT "/"

int verbose_mode = 0;
int assume_yes = 0;
int quiet_mode = 0;
int download_only = 0;
int simulate = 0;

typedef struct {
  char *repos[MAX_REPOS];
  int repo_count;
  int repo_priority[MAX_REPOS];
} VicPkgContext;

typedef struct {
  char package[256];
  char version[64];
  char architecture[64];
  char filename[512];
  char description[512];
  char name[256];
  long size;
  int is_legacy;
} PackageInfo;

void set_cpu_freq(const char *freq) {
  FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "w");
  if (f) {
    fprintf(f, "%s", freq);
    fclose(f);
  }
}

char *exec_command(const char *cmd) {
  FILE *fp = popen(cmd, "r");
  if (!fp)
    return NULL;

  static char result[1024];
  if (fgets(result, sizeof(result), fp) != NULL) {
    size_t len = strlen(result);
    if (len > 0 && result[len - 1] == '\n') {
      result[len - 1] = '\0';
    }
    pclose(fp);
    return result;
  }
  pclose(fp);
  return NULL;
}

void ensure_path_configured() {
  
  char *path_env = getenv("PATH");
  if (path_env && strstr(path_env, BIN_DIR)) {
    return; 
  }

  const char *profile_file = "/etc/profile";
  const char *path_line = "\nexport PATH=\"/data/vicpkg/bin:$PATH\"\n";

  FILE *f = fopen(profile_file, "r");
  if (f) {
    char line[MAX_LINE];
    int already_added = 0;
    while (fgets(line, sizeof(line), f)) {
      if (strstr(line, "/data/vicpkg/bin")) {
        already_added = 1;
        break;
      }
    }
    fclose(f);

    if (!already_added) {
      f = fopen(profile_file, "a");
      if (f) {
        fprintf(f, "%s", path_line);
        fclose(f);
        if (verbose_mode) {
          printf("[VERBOSE] Added %s to PATH in %s\n", BIN_DIR, profile_file);
        }
      }
    }
  }

  
  char new_path[4096];
  if (path_env) {
    snprintf(new_path, sizeof(new_path), "%s:%s", BIN_DIR, path_env);
  } else {
    snprintf(new_path, sizeof(new_path), "%s:/usr/bin:/bin", BIN_DIR);
  }
  setenv("PATH", new_path, 1);
}

void init_directories() {
  mkdir(VICPKG_DIR, 0755);
  mkdir(VERSIONS_DIR, 0755);
  mkdir(FILES_DIR, 0755);
  mkdir(CACHE_DIR, 0755);
  mkdir(BIN_DIR, 0755);
  
  char legacy_dir[MAX_PATH];
  snprintf(legacy_dir, sizeof(legacy_dir), "%s/legacy", VICPKG_DIR);
  mkdir(legacy_dir, 0755);
  mkdir(LEGACY_INSTALL_DIR, 0755);

  ensure_path_configured();
}

void init_vicpkg_self() {
  char version_file[MAX_PATH];
  snprintf(version_file, sizeof(version_file), "%s/vicpkg", VERSIONS_DIR);

  if (access(version_file, F_OK) != 0) {

    FILE *f = fopen(version_file, "w");
    if (f) {
      fprintf(f, "%s\n", VICPKG_VERSION);
      fclose(f);
    }

    char files_list[MAX_PATH];
    snprintf(files_list, sizeof(files_list), "%s/vicpkg", FILES_DIR);
    f = fopen(files_list, "w");
    if (f) {
      fprintf(f, "/anki/bin/vicpkg\n");
      fclose(f);
    }
  }
}

void load_repositories(VicPkgContext *ctx) {
  ctx->repo_count = 0;

  FILE *f = fopen(REPOS_FILE, "r");
  if (f) {
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f) && ctx->repo_count < MAX_REPOS) {
      size_t len = strlen(line);
      if (len > 0 && line[len - 1] == '\n') {
        line[len - 1] = '\0';
      }

      if (line[0] != '\0' && line[0] != '#') {
        ctx->repos[ctx->repo_count] = strdup(line);
        ctx->repo_priority[ctx->repo_count] = 0;
        ctx->repo_count++;
      }
    }
    fclose(f);
  }

  if (ctx->repo_count == 0) {
        ctx->repos[0] = strdup("https://www.froggitti.net/vector-mirror");
        ctx->repo_priority[0] = 0;
        ctx->repos[1] = strdup("https://raw.githubusercontent.com/Lrdsnow/snowypurplpkgrepo/refs/heads/main");
        ctx->repo_priority[1] = 0;
        ctx->repos[2] = strdup("https://raw.githubusercontent.com/Lrdsnow/vicpkg/refs/heads/main/repo");
        ctx->repo_priority[2] = 100;
        ctx->repo_count = 3;
  }
}

int check_repo_release(const char *repo) {
  char cmd[MAX_PATH * 2];
  char cache_file[MAX_PATH];

  snprintf(cache_file, sizeof(cache_file), "%s/release.tmp", CACHE_DIR);
  snprintf(cmd, sizeof(cmd), "curl -s -o %s %s/Release 2>/dev/null", cache_file,
           repo);

  if (system(cmd) != 0) {
    return 0;
  }

  FILE *f = fopen(cache_file, "r");
  if (!f) {
    return 0;
  }

  char line[MAX_LINE];
  int found_vicpkg = 0;

  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "Architectures:", 14) == 0) {
      if (strstr(line, "vicpkg")) {
        found_vicpkg = 1;
        break;
      }
    }
  }

  fclose(f);
  remove(cache_file);

  if (verbose_mode && found_vicpkg) {
    printf("[VERBOSE] Found valid Release file at %s\n", repo);
  }

  return found_vicpkg;
}

void prioritize_repos(VicPkgContext *ctx) {
  for (int i = 0; i < ctx->repo_count; i++) {
    if (check_repo_release(ctx->repos[i])) {
      ctx->repo_priority[i] = 100;
    } else {
      ctx->repo_priority[i] = 1;
    }
  }

  for (int i = 0; i < ctx->repo_count - 1; i++) {
    for (int j = 0; j < ctx->repo_count - i - 1; j++) {
      if (ctx->repo_priority[j] < ctx->repo_priority[j + 1]) {
        char *temp_repo = ctx->repos[j];
        ctx->repos[j] = ctx->repos[j + 1];
        ctx->repos[j + 1] = temp_repo;

        int temp_prio = ctx->repo_priority[j];
        ctx->repo_priority[j] = ctx->repo_priority[j + 1];
        ctx->repo_priority[j + 1] = temp_prio;
      }
    }
  }
}

void init_context(VicPkgContext *ctx) {
  ctx->repo_count = 0;

  init_directories();
  init_vicpkg_self();
  load_repositories(ctx);
  prioritize_repos(ctx);

  set_cpu_freq("1267200");
}

void cleanup_context(VicPkgContext *ctx) {
  for (int i = 0; i < ctx->repo_count; i++) {
    free(ctx->repos[i]);
  }
  set_cpu_freq("533333");
}

void show_usage() {
  printf("Usage: vicpkg [options] command\n");
  printf("\n");
  printf("Commands:\n");
  printf("  update                             - Update package cache\n");
  printf(
      "  upgrade [package]                  - Upgrade installed package(s)\n");
  printf("  install <package> [package2...]    - Install package(s)\n");
  printf("  purge <package> [package2...]     - Remove package(s)\n");
  printf("  search <query>                     - Search for packages\n");
  printf("  list                               - List installed packages\n");
  printf("  show <package>                     - Show package details\n");
  printf(
      "  repo-list                          - List configured repositories\n");
  printf("  repo-add <url>                     - Add a repository\n");
  printf("  repo-remove <url>                  - Remove a repository\n");
  printf("\n");
  printf("Options:\n");
  printf("  -h, --help           Show this help message\n");
  printf("  -v, --verbose        Verbose mode\n");
  printf("  -y, --yes            Assume yes to all prompts\n");
  printf("  -q, --quiet          Quiet mode\n");
  printf("  -s, --simulate       Simulate actions (dry-run)\n");
  printf("  -d, --download-only  Download packages only, don't install\n");
  printf("  --version            Show version information\n");
}

void show_version() { printf("vicpkg version %s\n", VICPKG_VERSION); }

int file_contains_404(const char *filepath) {
  FILE *f = fopen(filepath, "r");
  if (!f)
    return 0;

  char buffer[256];
  int found = 0;
  while (fgets(buffer, sizeof(buffer), f)) {
    if (strstr(buffer, "<head><title>404 Not Found</title></head>") ||
        strstr(buffer, "<!DOCTYPE HTML>")) {
      found = 1;
      break;
    }
  }
  fclose(f);
  return found;
}

int is_text_file(const char *filepath) {
  FILE *f = fopen(filepath, "rb");
  if (!f)
    return 0;

  unsigned char buffer[512];
  size_t bytes_read = fread(buffer, 1, sizeof(buffer), f);
  fclose(f);

  
  for (size_t i = 0; i < bytes_read; i++) {
    if (buffer[i] == 0) {
      return 0;
    }
  }

  return 1;
}

void patch_file_paths(const char *filepath, const char *package_dir) {
  if (!is_text_file(filepath)) {
    return;
  }

  char temp_file[MAX_PATH];
  snprintf(temp_file, sizeof(temp_file), "%s.tmp", filepath);

  FILE *src = fopen(filepath, "r");
  FILE *dst = fopen(temp_file, "w");

  if (!src || !dst) {
    if (src) fclose(src);
    if (dst) fclose(dst);
    return;
  }

  char line[MAX_LINE];
  int patched = 0;

  while (fgets(line, sizeof(line), src)) {
    char *pos = strstr(line, "/data/purplpkg");
    if (pos) {
      
      while (pos) {
        size_t prefix_len = pos - line;
        fwrite(line, 1, prefix_len, dst);
        fprintf(dst, "%s", package_dir);
        
        char *rest = pos + strlen("/data/purplpkg");
        strcpy(line, rest);
        pos = strstr(line, "/data/purplpkg");
        patched = 1;
      }
      fprintf(dst, "%s", line);
    } else {
      fprintf(dst, "%s", line);
    }
  }

  fclose(src);
  fclose(dst);

  if (patched) {
    if (verbose_mode) {
      printf("[VERBOSE] Patched paths in: %s\n", filepath);
    }
    rename(temp_file, filepath);
  } else {
    remove(temp_file);
  }
}

void create_symlinks_for_package(const char *package_name, const char *install_dir) {
  DIR *dir = opendir(install_dir);
  if (!dir) {
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.') {
      continue;
    }

    char source[MAX_PATH];
    char target[MAX_PATH];
    snprintf(source, sizeof(source), "%s/%s", install_dir, entry->d_name);
    snprintf(target, sizeof(target), "%s/%s", BIN_DIR, entry->d_name);

    
    unlink(target);

    
    if (symlink(source, target) == 0) {
      if (verbose_mode) {
        printf("[VERBOSE] Created symlink: %s -> %s\n", target, source);
      }
    } else if (verbose_mode) {
      printf("[VERBOSE] Failed to create symlink for %s\n", entry->d_name);
    }
  }

  closedir(dir);
}

void trim_string(char *str) {
  char *end;
  while (isspace((unsigned char)*str))
    str++;
  if (*str == 0)
    return;
  end = str + strlen(str) - 1;
  while (end > str && isspace((unsigned char)*end))
    end--;
  end[1] = '\0';
}

void remove_symlinks_for_package(const char *package_name) {
  char files_list[MAX_PATH];
  snprintf(files_list, sizeof(files_list), "%s/%s", FILES_DIR, package_name);

  FILE *f = fopen(files_list, "r");
  if (!f)
    return;

  char line[MAX_PATH];
  while (fgets(line, sizeof(line), f)) {
    trim_string(line);
    if (line[0] == '\0')
      continue;

    
    char *filename = strrchr(line, '/');
    if (filename) {
      filename++; 
      
      char symlink_path[MAX_PATH];
      snprintf(symlink_path, sizeof(symlink_path), "%s/%s", BIN_DIR, filename);
      
      
      struct stat st;
      if (lstat(symlink_path, &st) == 0 && S_ISLNK(st.st_mode)) {
        unlink(symlink_path);
        if (verbose_mode) {
          printf("[VERBOSE] Removed symlink: %s\n", symlink_path);
        }
      }
    }
  }

  fclose(f);
}

char *detect_compression(const char *filepath) {
  FILE *f = fopen(filepath, "rb");
  if (!f)
    return "unknown";

  unsigned char magic[4];
  size_t read = fread(magic, 1, 4, f);
  fclose(f);

  if (read < 2)
    return "unknown";

  if (magic[0] == 0x1f && magic[1] == 0x8b)
    return "gzip";
  if (magic[0] == 0x42 && magic[1] == 0x5a)
    return "bzip2";
  if (magic[0] == 0xfd && magic[1] == 0x37)
    return "xz";
  if (read >= 4 && magic[0] == 0x28 && magic[1] == 0xb5 && magic[2] == 0x2f &&
      magic[3] == 0xfd)
    return "zstd";

  return "gzip";
}

int extract_legacy_package(const char *package_file, const char *package_name) {
  char *compression = detect_compression(package_file);
  char cmd[MAX_PATH * 2];

  if (verbose_mode) {
    printf("[VERBOSE] Extracting legacy package with compression type: %s\n", compression);
  }

  char temp_dir[MAX_PATH];
  snprintf(temp_dir, sizeof(temp_dir), "/tmp/vicpkg_extract_%s", package_name);

  snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
  system(cmd);

  mkdir(temp_dir, 0755);

  
  if (strcmp(compression, "gzip") == 0) {
    snprintf(cmd, sizeof(cmd), "tar -xzf %s -C %s 2>/dev/null", package_file, temp_dir);
  } else if (strcmp(compression, "bzip2") == 0) {
    snprintf(cmd, sizeof(cmd), "tar -xjf %s -C %s 2>/dev/null", package_file, temp_dir);
  } else if (strcmp(compression, "xz") == 0) {
    snprintf(cmd, sizeof(cmd), "tar -xJf %s -C %s 2>/dev/null", package_file, temp_dir);
  } else if (strcmp(compression, "zstd") == 0) {
    snprintf(cmd, sizeof(cmd), "tar --zstd -xf %s -C %s 2>/dev/null", package_file, temp_dir);
  } else {
    snprintf(cmd, sizeof(cmd), "tar -xzf %s -C %s 2>/dev/null", package_file, temp_dir);
  }

  if (verbose_mode) {
    printf("[VERBOSE] Running: %s\n", cmd);
  }

  if (system(cmd) != 0) {
    fprintf(stderr, "Failed to extract archive\n");
    snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
    system(cmd);
    return 0;
  }

  
  char install_dir[MAX_PATH];
  snprintf(install_dir, sizeof(install_dir), "%s/%s", LEGACY_INSTALL_DIR, package_name);
  
  snprintf(cmd, sizeof(cmd), "rm -rf %s", install_dir);
  system(cmd);
  
  mkdir(install_dir, 0755);

  
  snprintf(cmd, sizeof(cmd), "mv %s/* %s/ 2>/dev/null", temp_dir, install_dir);
  if (system(cmd) != 0) {
    fprintf(stderr, "Failed to move files to install directory\n");
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", temp_dir, install_dir);
    system(cmd);
    return 0;
  }

  
  snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
  system(cmd);

  
  DIR *dir = opendir(install_dir);
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      if (entry->d_name[0] != '.') {
        char filepath[MAX_PATH];
        snprintf(filepath, sizeof(filepath), "%s/%s", install_dir, entry->d_name);
        
        struct stat st;
        if (stat(filepath, &st) == 0 && S_ISREG(st.st_mode)) {
          patch_file_paths(filepath, install_dir);
        }
      }
    }
    closedir(dir);
  }

  dir = opendir(install_dir);
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      if (entry->d_name[0] != '.') {
        char filepath[MAX_PATH];
        snprintf(filepath, sizeof(filepath), "%s/%s", install_dir, entry->d_name);
        
        struct stat st;
        if (stat(filepath, &st) == 0 && S_ISREG(st.st_mode)) {
          chmod(filepath, 0755);
          if (verbose_mode) {
            printf("[VERBOSE] Set executable permissions: %s\n", filepath);
          }
        }
      }
    }
    closedir(dir);
  }

  
  create_symlinks_for_package(package_name, install_dir);

  
  char files_list[MAX_PATH];
  snprintf(files_list, sizeof(files_list), "%s/%s", FILES_DIR, package_name);
  
  FILE *flist = fopen(files_list, "w");
  if (flist) {
    dir = opendir(install_dir);
    if (dir) {
      struct dirent *entry;
      while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') {
          fprintf(flist, "%s/%s\n", install_dir, entry->d_name);
        }
      }
      closedir(dir);
    }
    fclose(flist);
  }

  if (verbose_mode) {
    printf("[VERBOSE] Legacy package installed to: %s\n", install_dir);
  }

  return 1;
}

int extract_package_to_root(const char *package_file) {
  char *compression = detect_compression(package_file);
  char cmd[MAX_PATH * 2];

  if (verbose_mode) {
    printf("[VERBOSE] Extracting with compression type: %s\n", compression);
  }

  char temp_dir[MAX_PATH];
  snprintf(temp_dir, sizeof(temp_dir), "%s/temp_extract", CACHE_DIR);

  snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
  system(cmd);

  mkdir(temp_dir, 0755);

  if (strcmp(compression, "gzip") == 0) {
    snprintf(cmd, sizeof(cmd), "tar -xzf %s -C %s 2>/dev/null", package_file,
             temp_dir);
  } else if (strcmp(compression, "bzip2") == 0) {
    snprintf(cmd, sizeof(cmd), "tar -xjf %s -C %s 2>/dev/null", package_file,
             temp_dir);
  } else if (strcmp(compression, "xz") == 0) {
    snprintf(cmd, sizeof(cmd), "tar -xJf %s -C %s 2>/dev/null", package_file,
             temp_dir);
  } else if (strcmp(compression, "zstd") == 0) {
    snprintf(cmd, sizeof(cmd), "tar --zstd -xf %s -C %s 2>/dev/null",
             package_file, temp_dir);
  } else {
    snprintf(cmd, sizeof(cmd), "tar -xzf %s -C %s 2>/dev/null", package_file,
             temp_dir);
  }

  if (system(cmd) != 0) {
    snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
    system(cmd);
    return 0;
  }

  char pkg_dir[MAX_PATH];
  snprintf(pkg_dir, sizeof(pkg_dir), "%s/pkg", temp_dir);

  snprintf(cmd, sizeof(cmd), "cp -rf %s/* %s/ 2>/dev/null", pkg_dir,
           INSTALL_ROOT);
  int result = system(cmd);

  if (result != 0) {
    snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
    system(cmd);
    return 0;
  }

  return 1;
}

int download_file(const char *url, const char *output) {
  char cmd[MAX_PATH * 2];
  if (quiet_mode) {
    snprintf(cmd, sizeof(cmd), "curl -s -o %s %s 2>/dev/null", output, url);
  } else {
    snprintf(cmd, sizeof(cmd), "curl -# -o %s %s 2>/dev/null", output, url);
  }
  return system(cmd) == 0;
}

int parse_packages_file(const char *packages_file, const char *package_name,
                        PackageInfo *info) {
  FILE *f = fopen(packages_file, "r");
  if (!f)
    return 0;

  char line[MAX_LINE];
  int in_package = 0;
  int found = 0;

  memset(info, 0, sizeof(PackageInfo));

  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '\n' || line[0] == '\r') {
      if (in_package && found)
        break;
      in_package = 0;
      continue;
    }

    char *colon = strchr(line, ':');
    if (!colon)
      continue;

    *colon = '\0';
    char *key = line;
    char *value = colon + 1;

    while (isspace((unsigned char)*value))
      value++;
    trim_string(value);

    if (strcmp(key, "Package") == 0) {
      in_package = 1;
      if (strcmp(value, package_name) == 0) {
        found = 1;
        strncpy(info->package, value, sizeof(info->package) - 1);
      }
    } else if (in_package && found) {
      if (strcmp(key, "Version") == 0) {
        strncpy(info->version, value, sizeof(info->version) - 1);
      } else if (strcmp(key, "Architecture") == 0) {
        strncpy(info->architecture, value, sizeof(info->architecture) - 1);
      } else if (strcmp(key, "Filename") == 0) {
        strncpy(info->filename, value, sizeof(info->filename) - 1);
      } else if (strcmp(key, "Description") == 0) {
        strncpy(info->description, value, sizeof(info->description) - 1);
      } else if (strcmp(key, "Name") == 0) {
        strncpy(info->name, value, sizeof(info->name) - 1);
      } else if (strcmp(key, "Size") == 0) {
        info->size = atol(value);
      }
    }
  }

  fclose(f);
  return found;
}

int try_download_package_vicpkg(const char *repo, const char *package,
                                PackageInfo *info) {
  char packages_file[MAX_PATH];
  char cmd[MAX_PATH * 2];

  snprintf(packages_file, sizeof(packages_file), "%s/Packages.tmp", CACHE_DIR);
  snprintf(cmd, sizeof(cmd), "curl -s -o %s %s/Packages 2>/dev/null",
           packages_file, repo);

  if (system(cmd) != 0) {
    return 0;
  }

  if (!parse_packages_file(packages_file, package, info)) {
    remove(packages_file);
    return 0;
  }

  remove(packages_file);

  if (strcmp(info->architecture, "vicpkg") != 0) {
    if (verbose_mode) {
      printf("[VERBOSE] Package architecture '%s' doesn't match 'vicpkg'\n",
             info->architecture);
    }
    return 0;
  }

  char url[MAX_PATH];
  char local_file[MAX_PATH];

  if (info->filename[0] == '.' && info->filename[1] == '/') {
    snprintf(url, sizeof(url), "%s/%s", repo, info->filename + 2);
  } else {
    snprintf(url, sizeof(url), "%s/%s", repo, info->filename);
  }

  snprintf(local_file, sizeof(local_file), "%s/%s.vpkg", CACHE_DIR, package);

  if (verbose_mode) {
    printf("[VERBOSE] Downloading from: %s\n", url);
  }

  if (!download_file(url, local_file)) {
    return 0;
  }

  if (file_contains_404(local_file)) {
    remove(local_file);
    return 0;
  }

  info->is_legacy = 0;
  return 1;
}

int try_find_package_in_cache(VicPkgContext *ctx, const char *package, PackageInfo *info) {
  for (int i = 0; i < ctx->repo_count; i++) {
    if (ctx->repo_priority[i] >= 100) {
      char packages_file[MAX_PATH];
      snprintf(packages_file, sizeof(packages_file), "%s/Packages_%d",
               CACHE_DIR, i);

      if (parse_packages_file(packages_file, package, info)) {
        info->is_legacy = 0;
        
        
        char url[MAX_PATH];
        char local_file[MAX_PATH];

        if (info->filename[0] == '.' && info->filename[1] == '/') {
          snprintf(url, sizeof(url), "%s/%s", ctx->repos[i], info->filename + 2);
        } else {
          snprintf(url, sizeof(url), "%s/%s", ctx->repos[i], info->filename);
        }

        snprintf(local_file, sizeof(local_file), "%s/%s.vpkg", CACHE_DIR, package);

        if (verbose_mode) {
          printf("[VERBOSE] Downloading from cache info: %s\n", url);
        }

        if (!download_file(url, local_file)) {
          continue;
        }

        if (file_contains_404(local_file)) {
          remove(local_file);
          continue;
        }

        return 1;
      }
    }
  }
  return 0;
}

int try_download_package_legacy(const char *repo, const char *package, PackageInfo *info) {
  char url[MAX_PATH];
  char version_url[MAX_PATH];
  char flist_url[MAX_PATH];
  char local_file[MAX_PATH];
  char version_file[MAX_PATH];
  char flist_file[MAX_PATH];

  snprintf(url, sizeof(url), "%s/%s/%s.ppkg", repo, package, package);
  snprintf(version_url, sizeof(version_url), "%s/%s/%s.version", repo, package, package);
  snprintf(flist_url, sizeof(flist_url), "%s/%s/%s.flist", repo, package, package);

  snprintf(local_file, sizeof(local_file), "%s/%s.ppkg", CACHE_DIR, package);
  snprintf(version_file, sizeof(version_file), "%s/%s.version.tmp", CACHE_DIR, package);
  snprintf(flist_file, sizeof(flist_file), "%s/%s.flist.tmp", CACHE_DIR, package);

  if (verbose_mode) {
    printf("[VERBOSE] Trying legacy download from: %s\n", url);
  }

  
  if (!download_file(url, local_file)) {
    return 0;
  }

  if (file_contains_404(local_file)) {
    remove(local_file);
    return 0;
  }

  
  download_file(version_url, version_file);
  download_file(flist_url, flist_file);

  
  strncpy(info->package, package, sizeof(info->package) - 1);
  strncpy(info->architecture, "legacy", sizeof(info->architecture) - 1);
  info->is_legacy = 1;

  
  FILE *vf = fopen(version_file, "r");
  if (vf) {
    if (fgets(info->version, sizeof(info->version), vf)) {
      trim_string(info->version);
    }
    fclose(vf);
  } else {
    strncpy(info->version, "unknown", sizeof(info->version) - 1);
  }

  return 1;
}

int is_in_path(const char *filepath) {
  char *path_env = getenv("PATH");
  if (!path_env)
    return 0;

  char path_copy[4096];
  strncpy(path_copy, path_env, sizeof(path_copy) - 1);
  path_copy[sizeof(path_copy) - 1] = '\0';

  char file_dir[MAX_PATH];
  strncpy(file_dir, filepath, sizeof(file_dir) - 1);
  char *last_slash = strrchr(file_dir, '/');
  if (last_slash) {
    *last_slash = '\0';
  } else {
    return 0;
  }

  char *dir = strtok(path_copy, ":");
  while (dir != NULL) {
    if (strcmp(dir, file_dir) == 0) {
      return 1;
    }
    dir = strtok(NULL, ":");
  }

  return 0;
}

void save_package_list(const char *package, const char *temp_dir) {
  char temp_list[MAX_PATH];
  char files_list[MAX_PATH];

  snprintf(temp_list, sizeof(temp_list), "%s/package.list", temp_dir);
  snprintf(files_list, sizeof(files_list), "%s/%s", FILES_DIR, package);

  FILE *src = fopen(temp_list, "r");
  if (src) {
    FILE *dst = fopen(files_list, "w");
    if (dst) {
      char line[MAX_PATH];
      while (fgets(line, sizeof(line), src)) {
        fputs(line, dst);
      }
      fclose(dst);
    }
    fclose(src);
  }
}

void check_path_warning(const char *package) {
  char files_list[MAX_PATH];
  snprintf(files_list, sizeof(files_list), "%s/%s", FILES_DIR, package);

  FILE *f = fopen(files_list, "r");
  if (!f)
    return;

  char line[MAX_PATH];
  int found_in_path = 0;

  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
    }

    if (access(line, X_OK) == 0) {
      if (is_in_path(line)) {
        found_in_path = 1;
      } else {
        if (access(line, F_OK) != 0) {
          printf("WARNING: Expected file not found: %s\n", line);
        }
      }
    }
  }
  fclose(f);

  if (!found_in_path) {
    printf("NOTE: Legacy package installed to: %s/%s/\n", 
           LEGACY_INSTALL_DIR, package);
    printf("      Executables are available in PATH via symlinks in %s\n", BIN_DIR);
  }
}

int prompt_yes_no(const char *question) {
  if (assume_yes)
    return 1;

  printf("%s [Y/n] ", question);
  fflush(stdout);

  char response[10];
  if (fgets(response, sizeof(response), stdin) == NULL) {
    return 0;
  }

  if (response[0] == '\n')
    return 1;

  return (response[0] == 'y' || response[0] == 'Y');
}

char *format_size(long bytes) {
  static char buffer[64];

  if (bytes < 1024) {
    snprintf(buffer, sizeof(buffer), "%ld B", bytes);
  } else if (bytes < 1024 * 1024) {
    snprintf(buffer, sizeof(buffer), "%.1f KB", bytes / 1024.0);
  } else {
    snprintf(buffer, sizeof(buffer), "%.1f MB", bytes / (1024.0 * 1024.0));
  }

  return buffer;
}

int cmd_update(VicPkgContext *ctx) {
  if (!quiet_mode)
    printf("Updating package cache...\n");

  for (int i = 0; i < ctx->repo_count; i++) {
    if (!quiet_mode)
      printf("Fetching from: %s\n", ctx->repos[i]);

    if (ctx->repo_priority[i] >= 100) {
      char packages_file[MAX_PATH];
      char cmd[MAX_PATH * 2];

      snprintf(packages_file, sizeof(packages_file), "%s/Packages_%d",
               CACHE_DIR, i);
      snprintf(cmd, sizeof(cmd), "curl -s -o %s %s/Packages 2>/dev/null",
               packages_file, ctx->repos[i]);
      
      int result = system(cmd);
      if (verbose_mode) {
        printf("[VERBOSE] Downloaded Packages file to %s (result: %d)\n", packages_file, result);
      }
    } else {
      char list_file[MAX_PATH];
      char cmd[MAX_PATH * 2];

      snprintf(list_file, sizeof(list_file), "%s/package_list_%d", CACHE_DIR,
               i);
      snprintf(cmd, sizeof(cmd), "curl -s -o %s %s/package.list 2>/dev/null",
               list_file, ctx->repos[i]);
      
      int result = system(cmd);
      if (verbose_mode) {
        printf("[VERBOSE] Downloaded package.list to %s (result: %d)\n", list_file, result);
      }
    }
  }

  if (!quiet_mode)
    printf("Package cache updated.\n");
  return 0;
}

int cmd_search(VicPkgContext *ctx, const char *query) {
  printf("Searching for: %s\n\n", query);

  int found_any = 0;

  for (int i = 0; i < ctx->repo_count; i++) {
    if (ctx->repo_priority[i] >= 100) {
      char packages_file[MAX_PATH];
      snprintf(packages_file, sizeof(packages_file), "%s/Packages_%d",
               CACHE_DIR, i);

      FILE *f = fopen(packages_file, "r");
      if (f) {
        char line[MAX_LINE];
        char current_package[256] = "";
        char current_desc[512] = "";
        char current_version[64] = "";

        while (fgets(line, sizeof(line), f)) {
          if (strncmp(line, "Package:", 8) == 0) {
            char *val = line + 8;
            while (isspace(*val))
              val++;
            trim_string(val);
            strncpy(current_package, val, sizeof(current_package) - 1);
          } else if (strncmp(line, "Version:", 8) == 0) {
            char *val = line + 8;
            while (isspace(*val))
              val++;
            trim_string(val);
            strncpy(current_version, val, sizeof(current_version) - 1);
          } else if (strncmp(line, "Description:", 12) == 0) {
            char *val = line + 12;
            while (isspace(*val))
              val++;
            trim_string(val);
            strncpy(current_desc, val, sizeof(current_desc) - 1);
          } else if ((line[0] == '\n' || line[0] == '\r') &&
                     current_package[0] != '\0') {
            if (strstr(current_package, query) || strstr(current_desc, query)) {
              printf("%s/%s (%s)\n", ctx->repos[i], current_package,
                     current_version);
              if (current_desc[0] != '\0') {
                printf("  %s\n", current_desc);
              }
              printf("\n");
              found_any = 1;
            }
            current_package[0] = '\0';
            current_desc[0] = '\0';
            current_version[0] = '\0';
          }
        }
        
        
        if (current_package[0] != '\0') {
          if (strstr(current_package, query) || strstr(current_desc, query)) {
            printf("%s/%s (%s)\n", ctx->repos[i], current_package,
                   current_version);
            if (current_desc[0] != '\0') {
              printf("  %s\n", current_desc);
            }
            printf("\n");
            found_any = 1;
          }
        }
        
        fclose(f);
      } else if (verbose_mode) {
        printf("[VERBOSE] Could not open %s\n", packages_file);
      }
    } else {
      char list_file[MAX_PATH];
      snprintf(list_file, sizeof(list_file), "%s/package_list_%d", CACHE_DIR,
               i);

      FILE *f = fopen(list_file, "r");
      if (f) {
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) {
          trim_string(line);
          if (strstr(line, query)) {
            printf("%s/%s\n", ctx->repos[i], line);
            found_any = 1;
          }
        }
        fclose(f);
      } else if (verbose_mode) {
        printf("[VERBOSE] Could not open %s\n", list_file);
      }
    }
  }

  if (!found_any) {
    printf("No packages found matching '%s'\n", query);
  }

  return 0;
}

int cmd_show(VicPkgContext *ctx, const char *package) {
  PackageInfo info;
  int found = 0;

  for (int i = 0; i < ctx->repo_count; i++) {
    if (ctx->repo_priority[i] >= 100) {
      char packages_file[MAX_PATH];
      snprintf(packages_file, sizeof(packages_file), "%s/Packages_%d",
               CACHE_DIR, i);

      if (parse_packages_file(packages_file, package, &info)) {
        found = 1;
        break;
      }
    }
  }

  if (!found) {
    printf("Package '%s' not found.\n", package);
    return 1;
  }

  printf("Package: %s\n", info.package);
  if (info.name[0] != '\0') {
    printf("Name: %s\n", info.name);
  }
  printf("Version: %s\n", info.version);
  printf("Architecture: %s\n", info.architecture);
  if (info.size > 0) {
    printf("Size: %s\n", format_size(info.size));
  }
  if (info.description[0] != '\0') {
    printf("Description: %s\n", info.description);
  }

  char version_file[MAX_PATH];
  snprintf(version_file, sizeof(version_file), "%s/%s", VERSIONS_DIR, package);
  if (access(version_file, F_OK) == 0) {
    FILE *f = fopen(version_file, "r");
    if (f) {
      char installed_ver[64];
      if (fgets(installed_ver, sizeof(installed_ver), f)) {
        trim_string(installed_ver);
        printf("Installed: %s\n", installed_ver);
      }
      fclose(f);
    }
  }

  return 0;
}

int cmd_list_repos(VicPkgContext *ctx) {
  printf("Configured repositories:\n");
  for (int i = 0; i < ctx->repo_count; i++) {
    const char *type = (ctx->repo_priority[i] >= 100) ? "vicpkg" : "legacy";
    printf("%d. %s [%s]\n", i + 1, ctx->repos[i], type);
  }
  return 0;
}

int cmd_add_repo(VicPkgContext *ctx, const char *url) {
  for (int i = 0; i < ctx->repo_count; i++) {
    if (strcmp(ctx->repos[i], url) == 0) {
      printf("Repository already exists.\n");
      return 1;
    }
  }

  if (ctx->repo_count >= MAX_REPOS) {
    fprintf(stderr, "Maximum number of repositories reached.\n");
    return 1;
  }

  ctx->repos[ctx->repo_count] = strdup(url);
  ctx->repo_priority[ctx->repo_count] = 0;
  ctx->repo_count++;

  FILE *f = fopen(REPOS_FILE, "a");
  if (f) {
    fprintf(f, "%s\n", url);
    fclose(f);
  }

  prioritize_repos(ctx);

  printf("Repository added: %s\n", url);
  printf("Run 'vicpkg update' to fetch package lists.\n");
  return 0;
}

int cmd_remove_repo(VicPkgContext *ctx, const char *url) {
  int found = -1;
  for (int i = 0; i < ctx->repo_count; i++) {
    if (strcmp(ctx->repos[i], url) == 0) {
      found = i;
      break;
    }
  }

  if (found == -1) {
    printf("Repository not found.\n");
    return 1;
  }

  free(ctx->repos[found]);
  for (int i = found; i < ctx->repo_count - 1; i++) {
    ctx->repos[i] = ctx->repos[i + 1];
    ctx->repo_priority[i] = ctx->repo_priority[i + 1];
  }
  ctx->repo_count--;

  FILE *f = fopen(REPOS_FILE, "w");
  if (f) {
    for (int i = 0; i < ctx->repo_count; i++) {
      fprintf(f, "%s\n", ctx->repos[i]);
    }
    fclose(f);
  }

  printf("Repository removed: %s\n", url);
  return 0;
}

int cmd_list_installed() {
  printf("Installed packages:\n");
  DIR *dir = opendir(FILES_DIR);
  if (!dir) {
    fprintf(stderr, "Failed to open %s\n", FILES_DIR);
    return 1;
  }

  struct dirent *entry;
  int count = 0;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] != '.') {
      char version_file[MAX_PATH];
      snprintf(version_file, sizeof(version_file), "%s/%s", VERSIONS_DIR,
               entry->d_name);

      FILE *f = fopen(version_file, "r");
      if (f) {
        char version[64];
        if (fgets(version, sizeof(version), f)) {
          trim_string(version);
          printf("  %s (%s)\n", entry->d_name, version);
        } else {
          printf("  %s\n", entry->d_name);
        }
        fclose(f);
      } else {
        printf("  %s\n", entry->d_name);
      }
      count++;
    }
  }
  closedir(dir);

  if (count == 0) {
    printf("  No packages installed.\n");
  }

  return 0;
}

int cmd_remove_package(const char *package) {
  char files_list[MAX_PATH];
  snprintf(files_list, sizeof(files_list), "%s/%s", FILES_DIR, package);

  if (access(files_list, F_OK) != 0) {
    printf("Package %s is not installed.\n", package);
    return 1;
  }

  if (strcmp(package, "vicpkg") == 0) {
    printf("Cannot remove vicpkg while using it.\n");
    return 1;
  }

  if (!prompt_yes_no("Do you want to continue?")) {
    printf("Abort.\n");
    return 1;
  }

  if (simulate) {
    printf("Would remove %s\n", package);
    return 0;
  }

  if (!quiet_mode)
    printf("Removing %s...\n", package);

  
  remove_symlinks_for_package(package);

  FILE *f = fopen(files_list, "r");
  if (f) {
    char line[MAX_PATH];
    while (fgets(line, sizeof(line), f)) {
      trim_string(line);
      if (line[0] != '\0') {
        if (verbose_mode) {
          printf("[VERBOSE] Removing: %s\n", line);
        }
        
        
        struct stat st;
        if (stat(line, &st) == 0 && S_ISDIR(st.st_mode)) {
          char cmd[MAX_PATH * 2];
          snprintf(cmd, sizeof(cmd), "rm -rf %s", line);
          system(cmd);
        } else {
          remove(line);
        }
      }
    }
    fclose(f);
  }

  remove(files_list);

  char version_file[MAX_PATH];
  snprintf(version_file, sizeof(version_file), "%s/%s", VERSIONS_DIR, package);
  remove(version_file);

  if (!quiet_mode)
    printf("Package %s removed.\n", package);
  return 0;
}

int cmd_install_package(VicPkgContext *ctx, const char *package) {
  PackageInfo info;
  int found = 0;

  char version_file[MAX_PATH];
  snprintf(version_file, sizeof(version_file), "%s/%s", VERSIONS_DIR, package);
  int is_installed = (access(version_file, F_OK) == 0);

  
  if (try_find_package_in_cache(ctx, package, &info)) {
    found = 1;
    if (verbose_mode) {
      printf("[VERBOSE] Found package in cache\n");
    }
  }

  
  if (!found) {
    for (int i = 0; i < ctx->repo_count; i++) {
      if (verbose_mode) {
        printf("[VERBOSE] Trying repository: %s (priority: %d)\n", ctx->repos[i],
               ctx->repo_priority[i]);
      }

      if (ctx->repo_priority[i] < 100) {
        if (try_download_package_legacy(ctx->repos[i], package, &info)) {
          found = 1;
          break;
        }
      }
    }
  }

  if (!found) {
    printf("Package %s not found in any repository.\n", package);
    printf("Try running 'vicpkg update' first.\n");
    return 1;
  }

  if (is_installed) {
    FILE *f = fopen(version_file, "r");
    if (f) {
      char current_ver[64];
      if (fgets(current_ver, sizeof(current_ver), f)) {
        trim_string(current_ver);
        if (strcmp(current_ver, info.version) == 0) {
          printf("%s is already the newest version (%s).\n", package,
                 info.version);
          fclose(f);
          return 0;
        }
        printf("The following packages will be upgraded:\n");
        printf("  %s (%s -> %s)\n", package, current_ver, info.version);
      }
      fclose(f);
    }
  } else {
    printf("The following NEW packages will be installed:\n");
    printf("  %s (%s)", package, info.version);
    if (info.size > 0) {
      printf(" [%s]", format_size(info.size));
    }
    printf("\n");
  }

  if (info.size > 0) {
    printf("Need to download %s of archives.\n", format_size(info.size));
  }

  if (!prompt_yes_no("Do you want to continue?")) {
    printf("Abort.\n");
    return 1;
  }

  if (simulate) {
    printf("Would install %s version %s\n", package, info.version);
    return 0;
  }

  if (!quiet_mode)
    printf("Installing %s (%s)...\n", package, info.version);

  char pkg_file[MAX_PATH];
  if (info.is_legacy) {
    snprintf(pkg_file, sizeof(pkg_file), "%s/%s.ppkg", CACHE_DIR, package);
  } else {
    snprintf(pkg_file, sizeof(pkg_file), "%s/%s.vpkg", CACHE_DIR, package);
  }

  if (download_only) {
    printf("Downloaded to: %s\n", pkg_file);
    return 0;
  }

  int extract_success = 0;
  
  if (info.is_legacy) {
    extract_success = extract_legacy_package(pkg_file, package);
  } else {
    char temp_dir[MAX_PATH];
    snprintf(temp_dir, sizeof(temp_dir), "%s/temp_extract", CACHE_DIR);
    
    extract_success = extract_package_to_root(pkg_file);
    
    if (extract_success) {
      save_package_list(package, temp_dir);
      
      char cmd[MAX_PATH];
      snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
      system(cmd);
    }
  }

  if (!extract_success) {
    fprintf(stderr, "Failed to extract %s\n", package);
    remove(pkg_file);
    return 1;
  }

  
  if (info.is_legacy) {
    char version_tmp[MAX_PATH];
    char flist_tmp[MAX_PATH];
    snprintf(version_tmp, sizeof(version_tmp), "%s/%s.version.tmp", CACHE_DIR, package);
    snprintf(flist_tmp, sizeof(flist_tmp), "%s/%s.flist.tmp", CACHE_DIR, package);
    remove(version_tmp);
    remove(flist_tmp);
  }

  FILE *f = fopen(version_file, "w");
  if (f) {
    fprintf(f, "%s\n", info.version);
    fclose(f);
  }

  remove(pkg_file);

  if (!quiet_mode)
    printf("Package %s installed successfully.\n", package);

  if (!quiet_mode && info.is_legacy)
    check_path_warning(package);

  return 0;
}

int cmd_upgrade_package(VicPkgContext *ctx, const char *package) {
  char version_file[MAX_PATH];
  snprintf(version_file, sizeof(version_file), "%s/%s", VERSIONS_DIR, package);

  if (access(version_file, F_OK) != 0) {
    printf("Package %s is not installed.\n", package);
    return 1;
  }

  return cmd_install_package(ctx, package);
}

int cmd_upgrade_all(VicPkgContext *ctx) {
  printf("Checking for upgrades...\n");

  DIR *dir = opendir(VERSIONS_DIR);
  if (!dir) {
    fprintf(stderr, "Failed to open %s\n", VERSIONS_DIR);
    return 1;
  }

  char packages[100][256];
  int package_count = 0;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] != '.') {
      strncpy(packages[package_count], entry->d_name, sizeof(packages[0]) - 1);
      package_count++;
    }
  }
  closedir(dir);

  if (package_count == 0) {
    printf("No packages installed.\n");
    return 0;
  }

  int upgrades = 0;
  for (int i = 0; i < package_count; i++) {
    PackageInfo info;
    int found = 0;

    for (int j = 0; j < ctx->repo_count; j++) {
      if (ctx->repo_priority[j] >= 100) {
        char packages_file[MAX_PATH];
        snprintf(packages_file, sizeof(packages_file), "%s/Packages_%d",
                 CACHE_DIR, j);

        if (parse_packages_file(packages_file, packages[i], &info)) {
          found = 1;
          break;
        }
      }
    }

    if (found) {
      char version_file[MAX_PATH];
      snprintf(version_file, sizeof(version_file), "%s/%s", VERSIONS_DIR,
               packages[i]);

      FILE *f = fopen(version_file, "r");
      if (f) {
        char current_ver[64];
        if (fgets(current_ver, sizeof(current_ver), f)) {
          trim_string(current_ver);
          if (strcmp(current_ver, info.version) != 0) {
            printf("  %s (%s -> %s)\n", packages[i], current_ver, info.version);
            upgrades++;
          }
        }
        fclose(f);
      }
    }
  }

  if (upgrades == 0) {
    printf("All packages are up to date.\n");
    return 0;
  }

  printf("%d package(s) can be upgraded.\n", upgrades);

  if (!prompt_yes_no("Do you want to continue?")) {
    printf("Abort.\n");
    return 1;
  }

  for (int i = 0; i < package_count; i++) {
    cmd_upgrade_package(ctx, packages[i]);
  }

  return 0;
}

int main(int argc, char *argv[]) {

  int arg_start = 1;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
      verbose_mode = 1;
    } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
      assume_yes = 1;
    } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
      quiet_mode = 1;
    } else if (strcmp(argv[i], "-s") == 0 ||
               strcmp(argv[i], "--simulate") == 0) {
      simulate = 1;
    } else if (strcmp(argv[i], "-d") == 0 ||
               strcmp(argv[i], "--download-only") == 0) {
      download_only = 1;
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      show_usage();
      return 0;
    } else if (strcmp(argv[i], "--version") == 0) {
      show_version();
      return 0;
    } else if (argv[i][0] != '-') {
      arg_start = i;
      break;
    }
  }

  if (arg_start >= argc) {
    show_usage();
    return 0;
  }

  VicPkgContext ctx;
  init_context(&ctx);

  if (chdir(VICPKG_DIR) != 0) {
    fprintf(stderr, "Failed to change to %s\n", VICPKG_DIR);
    cleanup_context(&ctx);
    return 1;
  }

  int result = 0;
  const char *action = argv[arg_start];

  if (strcmp(action, "update") == 0) {
    result = cmd_update(&ctx);
  } else if (strcmp(action, "upgrade") == 0) {
    if (arg_start + 1 < argc && argv[arg_start + 1][0] != '-') {
      result = cmd_upgrade_package(&ctx, argv[arg_start + 1]);
    } else {
      result = cmd_upgrade_all(&ctx);
    }
  } else if (strcmp(action, "search") == 0 && arg_start + 1 < argc) {
    result = cmd_search(&ctx, argv[arg_start + 1]);
  } else if (strcmp(action, "show") == 0 && arg_start + 1 < argc) {
    result = cmd_show(&ctx, argv[arg_start + 1]);
  } else if (strcmp(action, "list") == 0) {
    result = cmd_list_installed();
  } else if (strcmp(action, "repo-list") == 0) {
    result = cmd_list_repos(&ctx);
  } else if (strcmp(action, "repo-add") == 0 && arg_start + 1 < argc) {
    result = cmd_add_repo(&ctx, argv[arg_start + 1]);
  } else if (strcmp(action, "repo-remove") == 0 && arg_start + 1 < argc) {
    result = cmd_remove_repo(&ctx, argv[arg_start + 1]);
  } else if (strcmp(action, "purge") == 0 && arg_start + 1 < argc) {
    for (int i = arg_start + 1; i < argc; i++) {
      if (argv[i][0] != '-') {
        if (cmd_remove_package(argv[i]) != 0) {
          result = 1;
        }
      }
    }
  } else if (strcmp(action, "install") == 0 && arg_start + 1 < argc) {
    for (int i = arg_start + 1; i < argc; i++) {
      if (argv[i][0] != '-') {
        if (cmd_install_package(&ctx, argv[i]) != 0) {
          result = 1;
        }
      }
    }
  } else {
    fprintf(stderr, "Unknown action or missing arguments: %s\n", action);
    show_usage();
    result = 1;
  }

  cleanup_context(&ctx);
  return result;
}
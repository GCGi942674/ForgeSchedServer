#!/usr/bin/env python3
import os, sys, re, pdb
import subprocess
import shutil
import tempfile
import zipfile
from contextlib import contextmanager

SDF_SORT_SCRIPT = "/home/zhenlei/projects/ForgeSched/sort_sdf.py"

GREEN = "\033[92m"
RED = "\033[91m"
BLUE = "\033[94m"
RESET = "\033[0m"


def clean_log(path: str):
  cleaned = []
  with open(path, "r", encoding="utf-8", errors="ignore") as f:
    for line in f:
      line = line.rstrip("\n")
      if not line.strip():
        continue
      if line.strip().startswith("general.maxThreads"):
        continue
      if "Errors encountered." in line:   #ignore maxThreads info, golden  is single-thread
        line = re.sub(r"\b\d+ Infos\b", "0 Infos", line)

      if line.startswith(("#Command route_design execution time:", "#Command place_design execution time:")) :
        break
      if line.startswith(("#", "Command:", "source", "set argv", "INFO")):
        continue
      if "Infos" in line and "Warnings" in line and "Errors" in line : # 9 Infos, 1 Warnings, 0 Critical Warnings and 0 Errors encountered.
        continue
      if line.startswith("Time taken to check if laguna hold fix is required") : # xcvu13p-fhgb2104-2-e_fip_cordic_rca
        continue
      if ("Time (s)" in line) or ("CPU" in line) or ("cpu" in line) or ("time:" in line) or ("xiaonan" in line):
        continue
      line = re.sub(r"\b\d\d:\d\d:\d\d\b", "00:00:00", line)
      cleaned.append(line)
  return cleaned


def clean_verilog(path: str):
  cleaned = []
  original = []
  found_timescale = False
  with open(path, "r", encoding="utf-8", errors="ignore") as f:
    for line in f:
      line = line.rstrip("\n")
      if line.strip():
        original.append(line)
      if not found_timescale:
        if not line.lstrip().startswith("`timescale"):
          continue
        found_timescale = True
      if line.strip():
        cleaned.append(line)
  return cleaned if found_timescale else original


def clean_sdf(path: str):
  cleaned = []
  original = []
  found_delayfile = False
  with open(path, "r", encoding="utf-8", errors="ignore") as f:
    for line in f:
      line = line.rstrip("\n")
      if re.match(r"^\(DATE(?:\s|\))", line.strip()):
        continue
      if line.strip():
        original.append(line)
      if not found_delayfile:
        if not line.lstrip().startswith("(DELAYFILE"):
          continue
        found_delayfile = True
      if line.strip():
        cleaned.append(line)
  return cleaned if found_delayfile else original


def clean_file(path: str):
  ext = os.path.splitext(path)[1].lower()
  if ext == ".v":
    return clean_verilog(path)
  elif ext == ".sdf":
    return clean_sdf(path)
  else:
    return clean_log(path)


@contextmanager
def prepare_file(path: str):
  if not path.lower().endswith(".zip"):
    yield path
    return

  with zipfile.ZipFile(path) as archive:
    files = [item for item in archive.infolist() if not item.is_dir()]
    expected_name = os.path.basename(path[:-4])
    matches = [item for item in files
               if os.path.basename(item.filename) == expected_name]
    if len(matches) == 1:
      member = matches[0]
    elif len(files) == 1:
      member = files[0]
    else:
      raise ValueError(f"Expected one file in ZIP archive: {path}")

    with tempfile.TemporaryDirectory() as temp_dir:
      prepared = os.path.join(temp_dir, os.path.basename(member.filename))
      with archive.open(member) as source, open(prepared, "wb") as target:
        shutil.copyfileobj(source, target)
      yield prepared


def sort_sdf_file(path: str, sorter: str, output: str):
  result = subprocess.run(
    [sys.executable, sorter, path, "-o", output],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True
  )
  if result.returncode != 0:
    raise RuntimeError(f"SDF sort failed for {path}: {result.stderr.strip()}")
  return output


def print_differences(log1, log2, f1, f2, to_diff):
  print("\n" + BLUE + "Differences:" + RESET)
  max_len = max(len(log1), len(log2))
  cnt = 1
  for i in range(max_len):
    line1 = log1[i] if i < len(log1) else ""
    line2 = log2[i] if i < len(log2) else ""
    if line1 != line2:
      if to_diff != "" :
        f = open(to_diff, "w" if cnt == 1 else "a")
        f.write(f"{cnt}:\n");
        f.write(f"[{f1}]\n")
        f.write("line: " + f"{line1}\n");
        f.write(f"[{f2}]\n")
        f.write("line: " + f"{line2}\n");
        f.write("\n")
        f.close()

      print(f"{cnt}:");
      print(RED + f"[{f1}]")
      print(BLUE + "line: " + f"{line1}");
      print(RED + f"[{f2}]")
      print(BLUE + "line: " + f"{line2}" + RESET);
      print()
      cnt += 1

def VvdCmpFiles(f1, f2, to_diff=""):
  with prepare_file(f1) as prepared1, prepare_file(f2) as prepared2:
    with tempfile.TemporaryDirectory() as sort_dir:
      if os.path.splitext(prepared1)[1].lower() == ".sdf":
        prepared1 = sort_sdf_file(
          prepared1, SDF_SORT_SCRIPT, os.path.join(sort_dir, "first_sorted.sdf")
        )
      if os.path.splitext(prepared2)[1].lower() == ".sdf":
        prepared2 = sort_sdf_file(
          prepared2, SDF_SORT_SCRIPT, os.path.join(sort_dir, "second_sorted.sdf")
        )
      log1 = clean_file(prepared1)
      log2 = clean_file(prepared2)

  if log1 == log2:
    print(GREEN + "Files are same" + RESET)
    return 1
  else:
    print(RED + "Files are different" + RESET)
    print_differences(log1, log2, f1, f2, to_diff)
    return 0


if __name__ == "__main__":
  if len(sys.argv) != 3:
    print("Usage: vvdCmpFiles.py <file1> <file2>")
    sys.exit(1)

  VvdCmpFiles(sys.argv[1], sys.argv[2])

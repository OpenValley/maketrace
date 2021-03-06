// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fromapt.h"

#include "tracer.h"
#include "analysis/configure.h"
#include "analysis/install.h"
#include "analysis/make.h"
#include "gen/bazel/generator.h"
#include "utils/logging.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>

FromApt::FromApt(const QString& package)
    : package_(package) {
  dir_.setAutoRemove(false);
  docker_dir_ = dir_.path() + "/docker";
  source_dir_ = dir_.path() + "/source";
  output_dir_ = dir_.path() + "/output";
  bazel_workspace_ = dir_.path() + "/workspace";

  QDir().mkdir(docker_dir_);
  QDir().mkdir(source_dir_);
  QDir().mkdir(output_dir_);
  QDir().mkdir(bazel_workspace_);
}

FromApt::Buildsystem FromApt::GuessBuildsystem() const {
  if (QFileInfo(source_dir_ + "/configure").isExecutable()) {
    return Buildsystem::Autotools;
  }
  if (QFile::exists(source_dir_ + "/CMakeLists.txt")) {
    return Buildsystem::CMake;
  }
  return Buildsystem::Unknown;
}

bool FromApt::Run() {
  CHECK(dir_.isValid());
  LOG(INFO) << "Using temporary directory " << dir_.path();

  // Write a dockerfile.
  QFile dockerfile(docker_dir_ + "/Dockerfile");
  dockerfile.open(QFile::WriteOnly);
  QTextStream ts(&dockerfile);
  ts << QString(
      "FROM ubuntu:trusty\n"
      "RUN mkdir /source /output\n"
      "RUN apt-get update && apt-get install -y build-essential\n"
      "RUN apt-get update && apt-get build-dep -y %1\n"
      "RUN cd /source && apt-get update && apt-get source %1\n"
      "RUN cp -ar /source/*/* /source/\n"
      "ADD tracer /usr/bin/\n"
      "WORKDIR /source\n").arg(package_);
  dockerfile.close();

  // Copy the tracer binary.
  QFile::copy(QCoreApplication::applicationFilePath(),
              docker_dir_ + "/tracer");

  // Build the docker container.
  QByteArray output;
  if (!RunCommand(docker_dir_, {"docker", "build", "."}) ||
      !RunCommand(docker_dir_, {"docker", "build", "."}, &output)) {
    LOG(ERROR) << "docker build failed";
    return false;
  }

  // Find the name of the image we just built.
  const QRegularExpression image_re("Successfully built ([a-f0-9]+)");
  const QRegularExpressionMatch match =
      image_re.match(QString::fromUtf8(output));
  if (!match.hasMatch()) {
    LOG(ERROR) << "couldn't find image hash in docker build output:" << output;
    return false;
  }
  image_ = match.captured(1);

  // Copy the source.
  if (!RunCommand(docker_dir_, {
                  "docker", "run",
                  "-v", source_dir_ + ":/mounted-source",
                  image_,
                  "bash", "-c", "cp -r /source/* /mounted-source/"})) {
    LOG(ERROR) << "rsync failed";
    return false;
  }

  // Run configure.
  if (!RunTracer({"trace", "/output/configure", "make", "-f", "debian/rules",
                  "configure"})) {
    LOG(ERROR) << "configure failed";
    return false;
  }
  if (!RunTracer({"trace", "/output/make", "make", "-f", "debian/rules",
                 "build"})) {
    LOG(ERROR) << "make failed";
    return false;
  }
  if (!RunTracer({"trace", "/output/install",  "make", "-f", "debian/rules",
                 "install"})) {
    LOG(ERROR) << "install failed";
    return false;
  }

  analysis::Configure::Options conf_opts;
  conf_opts.trace_filename = output_dir_ + "/configure.trace";
  conf_opts.output_filename = output_dir_ + "/configure.files";
  if (!analysis::Configure::Run(conf_opts)) {
    return false;
  }

  analysis::Install::Options install_opts;
  install_opts.trace_filename = output_dir_ + "/install.trace";
  install_opts.output_filename = output_dir_ + "/install.files";
  if (!analysis::Install::Run(install_opts)) {
    return false;
  }

  analysis::Make::Options make_opts;
  make_opts.trace_filename = output_dir_ + "/make.trace";
  make_opts.install_filename = install_opts.output_filename;
  make_opts.output_filename = output_dir_ + "/make.targets";
  make_opts.graph_output_filename = output_dir_ + "/make.dot";
  make_opts.intermediate_graph_output_filename =
      output_dir_ + "/make.intermediate.dot";
  if (!analysis::Make::Run(make_opts)) {
    return false;
  }

  gen::bazel::Generator::Options bazel_opts;
  bazel_opts.target_filename = make_opts.output_filename;
  bazel_opts.installed_files_filename = install_opts.output_filename;
  bazel_opts.workspace_path = bazel_workspace_;
  bazel_opts.project_root = source_dir_;
  if (!gen::bazel::Generator::Run(bazel_opts)) {
    return false;
  }

  return true;
}

bool FromApt::RunCommand(const QString& working_directory,
                         const QStringList& args,
                         QByteArray* output) const {
  LOG(INFO) << "Running " << args << " in " << working_directory;
  QProcess proc;
  proc.setWorkingDirectory(working_directory);
  proc.setProgram(args[0]);
  proc.setArguments(args.mid(1));
  if (output) {
    proc.setProcessChannelMode(QProcess::ForwardedErrorChannel);
  } else {
    proc.setProcessChannelMode(QProcess::ForwardedChannels);
  }
  proc.start();
  proc.waitForStarted(-1);
  if (proc.state() != QProcess::Running) {
    return false;
  }
  proc.waitForFinished(-1);
  if (output) {
    *output = proc.readAllStandardOutput();
  }
  return proc.exitCode() == 0;
}

bool FromApt::RunTracer(const QStringList& args, QByteArray* output) const {
  QStringList all_args = {
      "docker", "run",
      "-v", output_dir_ + ":/output",
      "-v", source_dir_ + ":/source",
  };
  if (args[0] == "trace") {
    all_args.append("--privileged");
  }
  all_args.append({
      image_,
      "/usr/bin/tracer",
      "--project_name", package_,
      "--",
  });
  all_args.append(args);
  return RunCommand(docker_dir_, all_args, output);
}

void FromApt::WriteEmptyShellScript(const QString& filename) const {
  if (!RunCommand(dir_.path(), {
      "docker", "run",
      "-v", source_dir_ + ":/source",
      image_,
      "bash", "-c",
      "echo '#!/bin/bash' > /source/" + filename,
  })) {
    LOG(ERROR) << "Failed to write empty shell script to " << filename;
  }
}

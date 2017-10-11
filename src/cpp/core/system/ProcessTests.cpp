/*
 * ProcessTests.cpp
 *
 * Copyright (C) 2017 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#ifndef _WIN32

#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <boost/thread.hpp>

#include <core/system/PosixProcess.hpp>
#include <core/system/PosixChildProcess.hpp>
#include <core/Thread.hpp>

#include <tests/TestThat.hpp>

namespace rstudio {
namespace core {
namespace system {
namespace tests {

void checkExitCode(int exitCode, int* outExitCode)
{
   *outExitCode = exitCode;
}

void signalExit(int exitCode, int* outExitCode, boost::mutex* mutex, boost::condition_variable* signal)
{
   *outExitCode = exitCode;

   LOCK_MUTEX(*mutex)
   {
      signal->notify_all();
   }
   END_LOCK_MUTEX
}

void appendOutput(const std::string& output, std::string* pOutput)
{
   pOutput->append(output);
}

struct IoServiceFixture
{
   boost::asio::io_service ioService;
   boost::asio::io_service::work work;
   boost::shared_ptr<boost::thread> pThread;

   void runServiceThread()
   {
      ioService.run();
   }

   IoServiceFixture() :
      ioService(), work(ioService),
      pThread(new boost::thread(boost::bind(&IoServiceFixture::runServiceThread, this)))
   {
   }

   ~IoServiceFixture()
   {
      ioService.stop();
      pThread->join();
   }
};

context("ProcessTests")
{
   test_that("AsioProcessSupervisor can run program")
   {
      IoServiceFixture fixture;

      // create new supervisor
      AsioProcessSupervisor supervisor(fixture.ioService);

      // create process options and callbacks
      ProcessOptions options;
      ProcessCallbacks callbacks;
      int exitCode = -1;
      callbacks.onExit = boost::bind(&checkExitCode, _1, &exitCode);

      // construct program arguments
      std::vector<std::string> args;
      args.push_back("Hello, world! This is a string to echo!");

      // run program
      supervisor.runProgram("/bin/echo", args, options, callbacks);

      // wait for it to exit
      bool success = supervisor.wait(boost::posix_time::seconds(5));

      // verify process exited successfully
      CHECK(success);
      CHECK(exitCode == 0);
   }

   test_that("AsioProcessSupervisor returns correct output from stdout")
   {
      IoServiceFixture fixture;

      // create new supervisor
      AsioProcessSupervisor supervisor(fixture.ioService);

      // create process options and callbacks
      ProcessOptions options;
      ProcessCallbacks callbacks;

      int exitCode = -1;
      std::string output;

      callbacks.onExit = boost::bind(&checkExitCode, _1, &exitCode);
      callbacks.onStdout = boost::bind(&appendOutput, _2, &output);

      // run command
      std::string command = "bash -c \"python -c $'for i in range(10):\n   print(i)'\"";
      supervisor.runCommand(command, options, callbacks);

      // wait for it to exit
      bool success = supervisor.wait(boost::posix_time::seconds(5));

      // verify process exited successfully and we got the expected output
      std::string expectedOutput = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n";
      CHECK(success);
      CHECK(exitCode == 0);
      CHECK(output == expectedOutput);
   }

   test_that("AsioProcessSupervisor returns correct error code for failure exit")
   {
      IoServiceFixture fixture;

      // create new supervisor
      AsioProcessSupervisor supervisor(fixture.ioService);

      // create process options and callbacks
      ProcessOptions options;
      ProcessCallbacks callbacks;

      int exitCode = -1;
      std::string output;

      callbacks.onExit = boost::bind(&checkExitCode, _1, &exitCode);

      // run command
      std::string command = "this is not a valid command";
      supervisor.runCommand(command, options, callbacks);

      // wait for it to exit
      bool success = supervisor.wait(boost::posix_time::seconds(5));

      CHECK(success);
      CHECK(exitCode == 127);
   }

   test_that("AsioAsyncChildProcess can write to std in")
   {
      IoServiceFixture fixture;

      ProcessOptions options;
      ProcessCallbacks callbacks;

      int exitCode = -1;
      std::string output;
      boost::condition_variable signal;
      boost::mutex mutex;

      callbacks.onExit = boost::bind(&signalExit, _1, &exitCode, &mutex, &signal);
      callbacks.onStdout = boost::bind(&appendOutput, _2, &output);

      AsioAsyncChildProcess proc(fixture.ioService, "cat", options);
      proc.run(callbacks);

      proc.asyncWriteToStdin("Hello\n", false);
      proc.asyncWriteToStdin("world!\n", true);

      std::string expectedOutput = "Hello\nworld!\n";

      boost::unique_lock<boost::mutex> lock(mutex);
      bool timedOut = !signal.timed_wait<boost::posix_time::seconds>(lock, boost::posix_time::seconds(5));

      CHECK(!timedOut);
      CHECK(exitCode == 0);
      CHECK(output == expectedOutput);
   }
}

} // end namespace tests
} // end namespace system
} // end namespace core
} // end namespace rstudio

#endif // !_WIN32

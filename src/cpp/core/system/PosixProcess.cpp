/*
 * PosixProcess.cpp
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

#include <core/system/PosixProcess.hpp>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/foreach.hpp>

#include <core/BoostThread.hpp>
#include <core/Thread.hpp>

#include <core/system/PosixChildProcess.hpp>

namespace rstudio {
namespace core {
namespace system {

// AsioProcessSupervisor currently on available on posix systems
struct AsioProcessSupervisor::Impl
{
   Impl(boost::asio::io_service& ioService) : ioService_(ioService) {}

   boost::asio::io_service& ioService_;
   std::set<boost::shared_ptr<AsioAsyncChildProcess> > children_;

   boost::mutex mutex_;
   boost::condition_variable noChildrenSignal_;

   Error runChild(boost::shared_ptr<AsioAsyncChildProcess> pChild, ProcessCallbacks callbacks)
   {
      // run the child
      Error error = pChild->run(callbacks);
      if (error)
         return error;

      LOCK_MUTEX(mutex_)
      {
         // add to the list of children
         children_.insert(pChild);
      }
      END_LOCK_MUTEX

      // success
      return Success();
   }

   bool hasRunningChildren()
   {
      LOCK_MUTEX(mutex_)
      {
         return !children_.empty();
      }
      END_LOCK_MUTEX

      // keep compiler happy
      return false;
   }

   void terminateAll()
   {
      // make a copy of our child map
      // we do this instead of iterating within a lock for processor efficiency
      // at a small cost to memory
      std::set<boost::shared_ptr<AsioAsyncChildProcess> > childCopies;
      LOCK_MUTEX(mutex_)
      {
         childCopies = children_;
      }
      END_LOCK_MUTEX

      // call terminate on all of our children
      BOOST_FOREACH(const boost::shared_ptr<AsioAsyncChildProcess>& pChild, childCopies)
      {
         Error error = pChild->terminate();
         if (error)
            LOG_ERROR(error);
      }
   }

   bool wait(const boost::posix_time::time_duration& maxWait)
   {
      boost::unique_lock<boost::mutex> lock(mutex_);

      if (children_.empty()) return true;

      // wait for all children to exit
      // note we have to use a different wait method
      // based on whether or not we should wait indefinitely
      if (maxWait.is_not_a_date_time())
      {
         noChildrenSignal_.wait(lock);
         return children_.empty();
      }
      else
         return noChildrenSignal_.timed_wait(lock, maxWait) && children_.empty();
   }

   void wrapExitCallback(boost::weak_ptr<AsioAsyncChildProcess> pChild,
                         int exitCode,
                         boost::function<void(int)> onExit)
   {
      // remove exited children
      // we lock here because this method can potentially be invoked by multiple threads
      // this is due to the fact that AsioAsyncChildProcess objects run on an io_serice
      LOCK_MUTEX(mutex_)
      {
         // upgrade this weak pointer to a shared pointer
         // this should be gauranteed to work but we do a null check just to be safe
         // this should always work because the actual child cannot die until we've erased it from
         // our collection. the weak pointer is used to ensure that the callbacks stored by the child
         // do not store a strong reference to itself, thus preventing the child from ever beeing freed
         boost::shared_ptr<AsioAsyncChildProcess> child = pChild.lock();
         if (child)
            children_.erase(child);
      }
      END_LOCK_MUTEX

      // invoke the user's requested callback
      if (onExit)
         onExit(exitCode);

      // finally, check to see if we have no children, and notify if so
      LOCK_MUTEX(mutex_)
      {
         if (children_.empty())
            noChildrenSignal_.notify_all();
      }
      END_LOCK_MUTEX
   }
};

AsioProcessSupervisor::AsioProcessSupervisor(boost::asio::io_service& ioService) :
   pImpl_(new Impl(ioService))
{
}

AsioProcessSupervisor::~AsioProcessSupervisor()
{
}

Error AsioProcessSupervisor::runProgram(const std::string& executable,
                                        const std::vector<std::string>& args,
                                        const ProcessOptions& options,
                                        const ProcessCallbacks& callbacks)
{
   // create the child
   boost::shared_ptr<AsioAsyncChildProcess> pChild(
            new AsioAsyncChildProcess(pImpl_->ioService_, executable, args, options));

   // wrap exit callback with our own so we reap dead child objects whenever they exit
   // note the use of the weak_ptr to ensure that the child's copy of the process callbacks
   // will not store a strong reference to itself, thus making it impossible to free
   ProcessCallbacks ourCallbacks = callbacks;
   ourCallbacks.onExit = boost::bind(&Impl::wrapExitCallback, pImpl_.get(),
                                     boost::weak_ptr<AsioAsyncChildProcess>(pChild), _1, callbacks.onExit);

   // run the child
   return pImpl_->runChild(pChild, ourCallbacks);
}

Error AsioProcessSupervisor::runCommand(const std::string& command,
                                        const ProcessOptions& options,
                                        const ProcessCallbacks& callbacks)
{
   // create the child
   boost::shared_ptr<AsioAsyncChildProcess> pChild(
            new AsioAsyncChildProcess(pImpl_->ioService_, command, options));

   // wrap exit callback with our own so we reap dead child objects whenever they exit
   // note the use of the weak_ptr to ensure that the child's copy of the process callbacks
   // will not store a strong reference to itself, thus making it impossible to free
   ProcessCallbacks ourCallbacks = callbacks;
   ourCallbacks.onExit = boost::bind(&Impl::wrapExitCallback, pImpl_.get(),
                                     boost::weak_ptr<AsioAsyncChildProcess>(pChild), _1, callbacks.onExit);

   // run the child
   return pImpl_->runChild(pChild, ourCallbacks);
}

bool AsioProcessSupervisor::hasRunningChildren()
{
   return pImpl_->hasRunningChildren();
}

void AsioProcessSupervisor::terminateAll()
{
   return pImpl_->terminateAll();
}

bool AsioProcessSupervisor::wait(const boost::posix_time::time_duration& maxWait)
{
   return pImpl_->wait(maxWait);
}

} // namespace system
} // namespace core
} // namespace rstudio

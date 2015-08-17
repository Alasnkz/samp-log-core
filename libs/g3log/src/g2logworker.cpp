/** ==========================================================================
 * 2011 by KjellKod.cc. This is PUBLIC DOMAIN to use at your own risk and comes
 * with no warranties. This code is yours to share, use and modify with no
 * strings attached and no restrictions or obligations.
 * 
 * For more information see g3log/LICENSE or refer refer to http://unlicense.org
 * ============================================================================
 * Filename:g2LogWorker.cpp  Framework for Logging and Design By Contract
 * Created: 2011 by Kjell Hedström
 *
 * PUBLIC DOMAIN and Not under copywrite protection. First published at KjellKod.cc
 * ********************************************* */

#include "g2logworker.hpp"
#include "g2logmessage.hpp"

#include <cassert>
#include <functional>
#include "active.hpp"
#include "g2log.hpp"
#include "g2time.hpp"
#include "g2future.hpp"
#include "crashhandler.hpp"
#include "std2_make_unique.hpp"
#include <iostream> // remove
#include <fstream>

namespace g2 {
	LogWorkerManager *LogWorkerManager::m_Instance = nullptr;

   LogWorkerImpl::LogWorkerImpl() : _bg(kjellkod::Active::createActive()) { }

   void LogWorkerImpl::bgSave(g2::LogMessagePtr msgPtr) {
      std::unique_ptr<LogMessage> uniqueMsg(std::move(msgPtr.get()));

      for (auto& sink : _sinks) {
         LogMessage msg(*(uniqueMsg));
         sink->send(LogMessageMover(std::move(msg)));
      }

      if (_sinks.empty()) {
         std::string err_msg{"g2logworker has no sinks. Message: ["};
         err_msg.append(uniqueMsg.get()->toString()).append({"]\n"});
         std::cerr << err_msg;
      }
   }

   void LogWorkerImpl::bgFatal(FatalMessagePtr msgPtr) {
      // this will be the last message. Only the active logworker can receive a FATAL call so it's 
      // safe to shutdown logging now

      std::string reason = msgPtr.get()->reason();
      const auto level = msgPtr.get()->_level;
      const auto fatal_id = msgPtr.get()->_signal_id;


	  g2::LogMessagePtr uniqueMsg { std2::make_unique<LogMessage>(std::move(*msgPtr.get())) };
	  LogWorkerManager::Get()->LogFatalMsg(uniqueMsg);
	  //TODO: format error message (more informations)

      /*uniqueMsg->write().append("\nExiting after fatal event  (").append(uniqueMsg->level());
     

     // Change output in case of a fatal signal (or windows exception)
      std::string exiting = {"Fatal type: "};

      uniqueMsg->write().append("). ").append(exiting).append(" ").append(reason)
              .append("\nLog content flushed sucessfully to sink\n\n");

      std::cerr << uniqueMsg->message() << std::flush;
      for (auto& sink : _sinks) {
         LogMessage msg(*(uniqueMsg));
         sink->send(LogMessageMover(std::move(msg)));
      }*/


      // This clear is absolutely necessary
      // All sinks are forced to receive the fatal message above before we continue
	  LogWorkerManager::Get()->ClearAllSinks();
      internal::exitWithDefaultSignalHandler(level, fatal_id);
      
      // should never reach this point
      perror("g2log exited after receiving FATAL trigger. Flush message status: ");
   }

   LogWorker::~LogWorker() {

     // The sinks WILL automatically be cleared at exit of this destructor
     // However, the waiting below ensures that all messages until this point are taken care of
     // before any internals/LogWorkerImpl of LogWorker starts to be destroyed. 
     // i.e. this avoids a race with another thread slipping through the "shutdownLogging" and calling
     // calling ::save or ::fatal through LOG/CHECK with lambda messages and "partly deconstructed LogWorkerImpl"
     //
     //   Any messages put into the queue will be OK due to:
     //  *) If it is before the wait below then they will be executed
     //  *) If it is AFTER the wait below then they will be ignored and NEVER executed
     auto bg_clear_sink_call = [this] { _impl._sinks.clear(); };
     auto token_cleared = g2::spawn_task(bg_clear_sink_call, _impl._bg.get());
     token_cleared.wait();

     // The background worker WILL be automatically cleared at the exit of the destructor
     // However, the explicitly clearing of the background worker (below) makes sure that there can
     // be no thread that manages to add another sink after the call to clear the sinks above. 
     //   i.e. this manages the extremely unlikely case of another thread calling
     // addWrappedSink after the sink clear above. Normally adding of sinks should be done in main.cpp
     // and be closely coupled with the existance of the LogWorker. Sharing this adding of sinks to 
     // other threads that do not know the state of LogWorker is considered a bug but it is dealt with 
     // nonetheless below.
     //
     // If sinks would already have been added after the sink clear above then this reset will deal with it 
     // without risking lambda execution with a partially deconstructed LogWorkerImpl
     // Calling g2::spawn_task on a nullptr Active object will not crash but return 
     // a future containing an appropriate exception. 
     _impl._bg.reset(nullptr);
   }
 
   void LogWorker::save(LogMessagePtr msg) {
	   LogMessagePtr msgCopy{ std2::make_unique<LogMessage>(*msg.get()) };

      _impl._bg->send([this, msg] {_impl.bgSave(msg); });

	  if (g2::logLevel(msgCopy.get()->_level, LOGLEVEL::ERROR))
		  LogWorkerManager::Get()->LogErrorMsg(msgCopy);
	  else if (g2::logLevel(msgCopy.get()->_level, LOGLEVEL::WARNING))
		  LogWorkerManager::Get()->LogWarningMsg(msgCopy);
   }

   void LogWorker::fatal(FatalMessagePtr fatal_message) {
      _impl._bg->send([this, fatal_message] {_impl.bgFatal(fatal_message); });}

   void LogWorker::addWrappedSink(std::shared_ptr<g2::internal::SinkWrapper> sink) {
      auto bg_addsink_call = [this, sink] {_impl._sinks.push_back(sink);};
      auto token_done = g2::spawn_task(bg_addsink_call, _impl._bg.get());
      token_done.wait();
   }


   namespace internal
   {
	   class CLogLevelSink
	   {
	   public:
		   CLogLevelSink(std::string loglevel) :
			   m_Logfile("logs/" + loglevel + ".log")
		   { }
		   void OnReceive(LogMessageMover m_msg)
		   {
			   LogMessage &msg = m_msg.get();
			   m_Logfile <<
				   "[" << msg.timestamp() << "] " <<
				   "[" << msg.module() << "] " <<
				   msg.message(); 
			   if (msg._line != 0)
			   {
				   m_Logfile << " (" << msg.file() << ":" << msg.line() << ")";
			   }
			   m_Logfile << '\n';
			   m_Logfile.flush();
		   }
	   private:
		   std::ofstream m_Logfile;

	   };
   }
   LogWorkerManager::LogWorkerManager()
   {
	   m_FatalLog->addSink(std2::make_unique<internal::CLogLevelSink>("fatal"), &internal::CLogLevelSink::OnReceive);
	   m_ErrorLog->addSink(std2::make_unique<internal::CLogLevelSink>("error"), &internal::CLogLevelSink::OnReceive);
	   m_WarningLog->addSink(std2::make_unique<internal::CLogLevelSink>("warning"), &internal::CLogLevelSink::OnReceive);
   }

} // g2

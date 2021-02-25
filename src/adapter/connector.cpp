//
// Copyright Copyright 2009-2021, AMT – The Association For Manufacturing Technology (“AMT”)
// All rights reserved.
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.
//

#include "connector.hpp"

#include <dlib/logger.h>

#include <chrono>
#include <utility>

using namespace std;
using namespace std::chrono;

namespace mtconnect
{
  namespace adapter
  {
    static dlib::logger g_logger("input.connector");

    // Connector public methods
    Connector::Connector(string server, unsigned int port, seconds legacyTimeout)
      : m_server(std::move(server)),
        m_port(port),
        m_localPort(0),
        m_connected(false),
        m_realTime(false),
        m_heartbeatFrequency {HEARTBEAT_FREQ},
        m_legacyTimeout(duration_cast<milliseconds>(legacyTimeout)),
        m_connectionActive(false)
    {
    }

    Connector::~Connector() { close(); }

    void Connector::connect()
    {
      m_connected = false;
      connecting();

      const char *ping = "* PING\n";

      try
      {
        m_connectionActive = true;

        // Connect to server:port, failure will throw dlib::socket_error exception
        // Using a smart pointer to ensure connection is deleted if exception thrown
        g_logger << LDEBUG << "Connecting to data source: " << m_server << " on port: " << m_port;
        m_connection.reset(dlib::connect(m_server, m_port));

        m_localPort = m_connection->get_local_port();

        // Check to see if this connection supports heartbeats.
        m_heartbeats = false;
        g_logger << LDEBUG << "(Port:" << m_localPort << ")"
                 << "Sending initial PING";
        auto status = m_connection->write(ping, strlen(ping));
        if (status < 0)
        {
          g_logger << LWARN << "(Port:" << m_localPort << ")"
                   << "connect: Could not write initial heartbeat: " << to_string(status);
          close();
          return;
        }

        connected();

        // If we have heartbeats, make sure we receive something every freq milliseconds.
        m_lastSent = m_lastHeartbeat = system_clock::now();

        // Make sure connection buffer is clear
        m_buffer.clear();

        // Socket buffer to put the extracted data into
        char sockBuf[SOCKET_BUFFER_SIZE + 1] = {0};

        // Keep track of the status return, else status = character bytes read
        // Assuming it always enters the while loop, it should never be 1
        m_connected = true;

        // Boost priority if this is a real-time adapter
        if (m_realTime)
        {
#ifdef _WINDOWS
          HANDLE hand = OpenThread(THREAD_ALL_ACCESS, FALSE, GetCurrentThreadId());
          SetThreadPriority(hand, THREAD_PRIORITY_TIME_CRITICAL);
          CloseHandle(hand);
#else
          struct sched_param param;
          param.sched_priority = 30;
          if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param))
            g_logger << LDEBUG << "Cannot set high thread priority";
#endif
        }
        g_logger << LTRACE << "(Port:" << m_localPort << ")"
                 << "Heartbeat : " << m_heartbeats;
        g_logger << LTRACE << "(Port:" << m_localPort << ")"
                 << "Heartbeat Freq: " << m_heartbeatFrequency.count();
        // Read from the socket, read is a blocking call
        while (m_connected)
        {
          auto         now = system_clock::now();
          milliseconds timeout(0);
          if (m_heartbeats)
          {
            timeout = m_heartbeatFrequency - duration_cast<milliseconds>(now - m_lastSent);
            g_logger << LTRACE << "(Port:" << m_localPort << ")"
                     << "Heartbeat Send Countdown: " << timeout.count();
          }
          else
          {
            timeout = m_legacyTimeout;
            g_logger << LTRACE << "(Port:" << m_localPort << ")"
                     << "Legacy Timeout: " << timeout.count();
          }

          if (timeout < milliseconds {0})
            timeout = milliseconds {1};
          sockBuf[0] = 0;

          if (m_connected)
            status = m_connection->read(sockBuf, SOCKET_BUFFER_SIZE, timeout.count());
          else
          {
            g_logger << LDEBUG << "(Port:" << m_localPort << ")"
                     << "Connection was closed, exiting adapter connect";
            break;
          }

          if (!m_connected)
          {
            g_logger << LDEBUG << "(Port:" << m_localPort << ")"
                     << "Connection was closed during read, exiting adapter";
            break;
          }

          if (status > 0)
          {
            // Give a null terminator for the end of buffer
            sockBuf[status] = '\0';
            parseBuffer(sockBuf);
          }
          else if (status == TIMEOUT && !m_heartbeats && (system_clock::now() - now) >= timeout)
          {
            // We don't stop on heartbeats, but if we have a legacy timeout, then we stop.
            g_logger << LERROR << "(Port:" << m_localPort << ")"
                     << "connect: Did not receive data for over: "
                     << duration_cast<seconds>(timeout).count() << " seconds";
            break;
          }
          else if (status != TIMEOUT)  // Something other than timeout occurred
          {
            g_logger << LERROR << "(Port:" << m_localPort << ")"
                     << "connect: Socket error, disconnecting";
            break;
          }

          if (m_heartbeats)
          {
            now = system_clock::now();
            if ((now - m_lastHeartbeat) > (m_heartbeatFrequency * 2))
            {
              g_logger << LERROR << "(Port:" << m_localPort << ")"
                       << "connect: Did not receive heartbeat for over: "
                       << (m_heartbeatFrequency * 2).count();
              break;
            }
            else if ((now - m_lastSent) >= m_heartbeatFrequency)
            {
              std::lock_guard<std::mutex> lock(m_commandLock);
              g_logger << LDEBUG << "(Port:" << m_localPort << ")"
                       << "Sending a PING for " << m_server << " on port " << m_port;
              status = m_connection->write(ping, strlen(ping));
              if (status <= 0)
              {
                g_logger << LERROR << "(Port:" << m_localPort << ")"
                         << "connect: Could not write heartbeat: " << status;
                break;
              }
              m_lastSent = now;
            }
          }
        }

        g_logger << LERROR << "(Port:" << m_localPort << ")"
                 << "connect: Connection exited with status: " << status;
      }
      catch (dlib::socket_error &e)
      {
        g_logger << LWARN << "(Port:" << m_localPort << ")"
                 << "connect: Socket exception: " << e.what();
      }
      catch (exception &e)
      {
        g_logger << LERROR << "(Port:" << m_localPort << ")"
                 << "connect: Exception in connect: " << e.what();
      }
      catch (...)
      {
        g_logger << LERROR << "(Port:" << m_localPort << ")"
                 << "connect: Unknown Exception in connect";
      }
      {
        std::unique_lock<std::mutex> lk(m_connectionMutex);
        m_connectionActive = false;
        lk.unlock();
        m_connectionCondition.notify_all();
      }
      close();
    }

    void Connector::parseBuffer(const char *buffer)
    {
      // Treat any data as a heartbeat.
      if (m_heartbeats)
        m_lastHeartbeat = system_clock::now();

      // Append the temporary buffer to the socket buffer
      m_buffer.append(buffer);

      auto newLine = m_buffer.find_last_of('\n');

      // Check to see if there is even a '\n' in buffer
      if (newLine != string::npos)
      {
        // If the '\n' is not at the end of the buffer, then save the overflow
        string overflow;

        if (newLine != m_buffer.length() - 1)
        {
          overflow = m_buffer.substr(newLine + 1);
          m_buffer = m_buffer.substr(0, newLine);
        }

        // TODO: Use string Views and skip getline. Use
        // find_first_of and then clip to subview of line.
        // Search the buffer for new complete lines
        string       line;
        stringstream stream(m_buffer, stringstream::in);

        while (!stream.eof())
        {
          getline(stream, line);
          g_logger << LTRACE << "(Port:" << m_localPort << ") Received line: '" << line << '\'';

          if (line.empty())
            continue;

          auto end = line.find_last_not_of(" \t\n\r");
          if (end != string::npos)
            line.erase(end + 1);

          // Check for heartbeats
          if (line[0] == '*')
          {
            if (!line.compare(0, 6, "* PONG"))
            {
              if (g_logger.level().priority <= LDEBUG.priority)
              {
                g_logger << LDEBUG << "(Port:" << m_localPort << ")"
                         << "Received a PONG for " << m_server << " on port " << m_port;
                auto delta = date::floor<milliseconds>(system_clock::now() - m_lastHeartbeat);
                g_logger << LDEBUG << "(Port:" << m_localPort << ")"
                         << "    Time since last heartbeat: " << delta.count() << "ms";
              }
              if (!m_heartbeats)
                startHeartbeats(line);
            }
            else
            {
              protocolCommand(line);
            }
          }
          else
          {
            processData(line);
          }
        }

        // Clear buffer/insert overflow data
        m_buffer = overflow;
      }
    }

    void Connector::sendCommand(const string &command)
    {
      std::lock_guard<std::mutex> lock(m_commandLock);

      if (m_connected)
      {
        string completeCommand = "* " + command + "\n";
        long   status = m_connection->write(completeCommand.c_str(), completeCommand.length());
        if (status <= 0)
        {
          g_logger << LWARN << "(Port:" << m_localPort << ")"
                   << "sendCommand: Could not write command: '" << command << "' - "
                   << to_string(status);
        }
      }
    }

    void Connector::startHeartbeats(const string &arg)
    {
      size_t pos;
      if (arg.length() > 7 && arg[6] == ' ' &&
          (pos = arg.find_first_of("0123456789", 7)) != string::npos)
      {
        auto              freq = milliseconds {atoi(arg.substr(pos).c_str())};
        constexpr minutes maxTimeOut = minutes {30};  // Make the maximum timeout 30 minutes.

        if (freq > 0ms && freq < maxTimeOut)
        {
          g_logger << LDEBUG << "(Port:" << m_localPort << ")"
                   << "Received PONG, starting heartbeats every " << freq.count() << "ms";
          m_heartbeats = true;
          m_heartbeatFrequency = freq;
        }
        else
        {
          g_logger << LERROR << "(Port:" << m_localPort << ")"
                   << "startHeartbeats: Bad heartbeat frequency " << arg << ", ignoring";
        }
      }
      else
      {
        g_logger << LERROR << "(Port:" << m_localPort << ")"
                 << "startHeartbeats: Bad heartbeat command " << arg << ", ignoring";
      }
    }

    void Connector::close()
    {
      if (m_connected && m_connection.get())
      {
        // Shutdown the socket and close the connection.
        try
        {
          m_connected = false;
          m_connection->shutdown();
        }
        catch (exception &e)
        {
          g_logger << LERROR << "(Port:" << m_localPort << ")"
                   << "close: Exception when shutting down connection: " << e.what();
        }

        g_logger << LDEBUG << "(Port:" << m_localPort << ")"
                 << "Waiting for connect method to exit and signal connection closed";

        if (m_connectionActive)
        {
          std::unique_lock<std::mutex> lk(m_connectionMutex);
          m_connectionCondition.wait(lk, [this]() { return !m_connectionActive; });
        }

        // Destroy the connection object.
        m_connection.reset();

        disconnected();
      }
    }
  }  // namespace adapter
}  // namespace mtconnect

#include <sstream>
#include <string>
#include <fstream>
#include <cstring>

#include "collectagent.h"
#include "asio.hpp"

unsigned int rstats_connection_id = 0;
unsigned int job_instance_id = 0;
unsigned int scenario_instance_id = 0;
unsigned int owner_scenario_instance_id = 0;
std::string agent_name("");
std::string job_name;


namespace collect_agent {

/*
 * Helpfull constants to easily create sockets to the local
 * RStats relay.
 */
asio::io_service io_service;
asio::ip::udp::resolver resolver(io_service);


inline std::string getenv(const char* name) {
#if defined _WIN32
  const unsigned int ENV_VALUE_LENGTH = 1000;
  char value[ENV_VALUE_LENGTH];
  unsigned int retrieved = GetEnvironmentVariableA(name, value, ENV_VALUE_LENGTH);
  if (!retrieved) {
    return std::string();
  }
#else
  const char* value = std::getenv(name);
  if (!value) {
    return std::string();
  }
#endif
  return value;
}

inline unsigned int from_env(const char* name, unsigned int default_value) {
  const std::string value = getenv(name);
  if (value.empty()) {
    return default_value;
  }
  std::stringstream parser;
  parser << value;
  unsigned int parsed;
  parser >> parsed;
  return parsed;
}


/*
 * Helper function to send a message to the local RStats relay.
 */
std::string rstats_messager(const std::string& message) {
  asio::error_code error;
  static asio::ip::udp::endpoint endpoint = *resolver.resolve(asio::ip::udp::resolver::query(asio::ip::udp::v4(), "", "1111"));

  // Connect to the RStats service and send our message
  asio::ip::udp::socket sock(io_service);
  sock.open(asio::ip::udp::v4());
  sock.send_to(asio::buffer(message), endpoint, 0, error);
  if (error) {
    send_log(LOG_ERR, "Error: Connexion to rstats refused, maybe rstats service isn't started");
    throw asio::system_error(error);
  }

  // Receive the response from the RStats service and
  // propagate it to the caller.
  char data[2048];
  sock.receive(asio::buffer(data), 0, error);  // TODO: See http://www.boost.org/doc/libs/1_58_0/doc/html/boost_asio/example/cpp03/timeouts/blocking_udp_client.cpp and implement a timeout
  if (error && error != asio::error::message_size) {
    send_log(LOG_ERR, "Error: Connexion to rstats was closed, could not get an answer");
    throw asio::system_error(error);
  }

  return std::string(data);
}

/*
 * Create the message to register and configure a new job;
 * send it to the RStats service and propagate its response.
 * Also open a syslog connection
 */
bool register_collect(
    const std::string& config_file,
    int log_option,
    int log_facility,
    bool _new) {
  // Get the ids
  job_name = getenv("JOB_NAME");
  if (job_name.empty()) {
    job_name = "job_debug";
  }
  job_instance_id = from_env("JOB_INSTANCE_ID", 0);
  scenario_instance_id = from_env("SCENARIO_INSTANCE_ID", 0);
  owner_scenario_instance_id = from_env("OWNER_SCENARIO_INSTANCE_ID", 0);
  std::ifstream agent_name_file;
  
  agent_name_file.open("/opt/openbach/agent/agent_name");
  if (agent_name_file.is_open()) {
    goto read;
  }

  agent_name_file.open("/etc/hostname");
  if (agent_name_file.is_open()) {
    goto read;
  }

  agent_name_file.open("C:\\openbach\\agent_name");
  if (!agent_name_file.is_open()) {
    agent_name = "agent_name_not_found";
  } else {
read:
    std::getline(agent_name_file, agent_name);
    agent_name_file.close();
  }

  // Open the log
  openlog(job_name.c_str(), log_option, log_facility);

  // Format the message to send to rstats
  std::stringstream command;
  command << "1 \"" << config_file << "\" \"" << job_name << "\" " << job_instance_id << " " << scenario_instance_id << " " << owner_scenario_instance_id << " \"" << agent_name << "\" " << _new;

  // Send the message to rstats
  std::string result;
  try {
    result = rstats_messager(command.str());
  } catch (std::exception& e) {
    send_log(LOG_ERR, "Failed to register to rstats service: %s", e.what());
    return false;
  }
  std::stringstream parser(result);

  // Format the response and propagate it
  std::string startswith;
  parser >> startswith;
  if (startswith == "OK") {
    unsigned int id;
    parser >> id;
    if (!id) {
      send_log(LOG_ERR, "ERROR: Return message isn't well formed");
      send_log(LOG_ERR, "\t%s", result.c_str());
    } else {
      send_log(LOG_NOTICE, "NOTICE: Connexion ID is %d", id);
    }
    rstats_connection_id = id;
    return true;
  } else if (startswith == "KO") {
    send_log(LOG_ERR, "ERROR: Something went wrong");
  } else {
    send_log(LOG_ERR, "ERROR: Return message isn't well formed");
  }

  rstats_connection_id = 0;
  send_log(LOG_ERR, "\t%s", result.c_str());
  return false;
}

/*
 * Send the log
 */
void send_log(
    int priority,
    const char* log,
    va_list ap) {
  // Create the message to log
  std::stringstream message;
  message
    << "OWNER_SCENARIO_INSTANCE_ID "
    << owner_scenario_instance_id
    << ", SCENARIO_INSTANCE_ID "
    << scenario_instance_id
    << ", JOB_INSTANCE_ID "
    << job_instance_id
    << ", AGENT_NAME "
    << agent_name
    << ", " << log;
  // Send the message
  vsyslog(priority, message.str().c_str(), ap);
}

/*
 * Send the log
 */
void send_log(
    int priority,
    const char* log,
    ...) {
  // Get the variable arguments
  va_list ap;
  va_start(ap, log);
  // Send the message
  send_log(priority, log, ap);
  va_end(ap);
}

/*
 * Create the message to generate a new statistic;
 * send it to the RStats service and propagate its response.
 */
std::string send_stat(
    long long timestamp,
    const std::unordered_map<std::string, std::string>& stats,
    const std::string& suffix) {
  // Format the message
  std::stringstream command;
  command << "2 " << rstats_connection_id << " " << timestamp;

  for (auto& stat : stats) {
    command << " \"" << stat.first << "\" \"" << stat.second << "\"";
  }
  if (suffix != "") {
    command << " " << suffix;
  }

  // Send the message and propagate RStats response
  try {
    return rstats_messager(command.str());
  } catch (std::exception& e) {
    std::string msg = "KO Failed to send statistic to rstats: ";
    msg += e.what();
    send_log(LOG_ERR, "%s", msg.c_str());
    return msg;
  }
}

/*
 * Helper function that mimics `send_stat` functionality with
 * statistics values already formatted.
 */
std::string send_prepared_stat(
    long long timestamp,
    const std::string& suffix,
    const std::string& stat_values) {
  // Format the message
  std::stringstream command;
  command << "2 " << rstats_connection_id << " " << timestamp;
  if (stat_values != "") {
    command << " " << stat_values;
  }
  if (suffix != "") {
    command << " " << suffix;
  }

  // Send the message and propagate RStats response
  try {
    return rstats_messager(command.str());
  } catch (std::exception& e) {
    std::string msg = "KO Failed to send statistic to rstats: ";
    msg += e.what();
    send_log(LOG_ERR, "%s", msg.c_str());
    return msg;
  }
}

/*
 * Create the message to reload a job configuration;
 * send it to the RStats service and propagate its response.
 */
std::string reload_stat() {
  // Format the message
  std::stringstream command;
  command << "3 " << rstats_connection_id;

  // Send the message and propagate RStats response
  try {
    return rstats_messager(command.str());
  } catch (std::exception& e) {
    std::string msg = "KO Failed to reload statistic: ";
    msg += e.what();
    send_log(LOG_ERR, "%s", msg.c_str());
    return msg;
  }
}

/*
 * Create the message to remove a registered job;
 * send it to the RStats service and propagate its response.
 */
std::string remove_stat() {
  // Format the message
  std::stringstream command;
  command << "4 " << rstats_connection_id;

  // Send the message and propagate RStats response
  try {
    return rstats_messager(command.str());
  } catch (std::exception& e) {
    std::string msg = "KO Failed to remove statistic: ";
    msg += e.what();
    send_log(LOG_ERR, "%s", msg.c_str());
    return msg;
  }
}

/*
 * Create the message to reload all jobs configurations at once;
 * send it to the RStats service and propagate its response.
 */
std::string reload_all_stats() {
  try {
    return rstats_messager("5");
  } catch (std::exception& e) {
    std::string msg = "KO Failed to reload statistics: ";
    msg += e.what();
    send_log(LOG_ERR, "%s", msg.c_str());
    return msg;
  }
}

/*
 * Create the message to fetch current jobs configurations;
 * send it to the RStats service and propagate its response.
 */
std::string change_config(bool storage, bool broadcast) {
  // Get the ids
  unsigned int job_id = from_env("JOB_INSTANCE_ID", 0);
  unsigned int scenario_id = from_env("SCENARIO_INSTANCE_ID", 0);

  // Format the message
  std::stringstream command;
  command << "6 " << scenario_id << " " << job_id << " " << storage << " " << broadcast;

  try {
    return rstats_messager(command.str());
  } catch (std::exception& e) {
    std::string msg = "KO Failed to fetch configurations: ";
    msg += e.what();
    send_log(LOG_ERR, "%s", msg.c_str());
    return msg;
  }
}

}

/*
 * Helper function to return a suitable type to the Python interperter.
 */
char* convert_std_string_to_char(const std::string& value) {
  char* result = new char[value.length()+1];
  std::memcpy(result, value.c_str(), sizeof(char) * value.length());
  result[value.length()] = 0;
  return result;
}

/*
 * Maps C interface to C++ call.
 */
unsigned int collect_agent_register_collect(
    char* config_file,
    int log_option,
    int log_facility,
    bool _new) {
  return collect_agent::register_collect(config_file, log_option, log_facility, _new);
}

/*
 * Maps C interface to C++ call.
 */
void collect_agent_send_log(
    int priority,
    const char* log,
    ...) {
  va_list ap;
  va_start(ap, log);
  collect_agent::send_log(priority, log, ap);
  va_end(ap);
}

/*
 * Maps C interface to C++ call.
 */
char* collect_agent_send_stat(
    long long timestamp,
    char* suffix,
    char* stats) {
  return convert_std_string_to_char(collect_agent::send_prepared_stat(timestamp, suffix, stats));
}

/*
 * Maps C interface to C++ call.
 */
char* collect_agent_reload_stat() {
  return convert_std_string_to_char(collect_agent::reload_stat());
}

/*
 * Maps C interface to C++ call.
 */
char* collect_agent_remove_stat() {
  return convert_std_string_to_char(collect_agent::remove_stat());
}

/*
 * Maps C interface to C++ call.
 */
char* collect_agent_reload_all_stats() {
  return convert_std_string_to_char(collect_agent::reload_all_stats());
}

/*
 * Maps C interface to C++ call.
 */
char* collect_agent_change_config(bool storage, bool broadcast) {
  return convert_std_string_to_char(collect_agent::change_config(storage, broadcast));
}

#include <fcntl.h>
#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <mysql.h>

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

void fetch(const sftp_session& sftp, const std::string& remotePath,
           const std::string& localPath) {
  sftp_file file = sftp_open(sftp, remotePath.c_str(), O_RDONLY, 0);

  std::size_t read = 0;
  const std::size_t BUFFER_SIZE = 1024;
  std::string buffer(BUFFER_SIZE, '\0');
  std::ofstream fout(localPath);

  while (read = sftp_read(file, (void*)buffer.data(), buffer.size())) {
    fout << std::string(buffer.begin(), buffer.begin() + read);
  }

  sftp_close(file);
}

int sftp_list_dir(ssh_session ssh, sftp_session sftp,
                  std::vector<std::string>& vec, std::string& rem,
                  std::string& loc) {
  sftp_dir dir;
  sftp_attributes attributes;
  int rc;

  dir = sftp_opendir(sftp, rem.c_str());

  if (!dir) {
    fprintf(stderr, "Directory not opened: %s\n", ssh_get_error(ssh));
    return SSH_ERROR;
  }

  while ((attributes = sftp_readdir(sftp, dir)) != NULL) {
    vec.push_back(attributes->name);

    fetch(sftp, rem + '/' + std::string(attributes->name),
          loc + '/' + std::string(attributes->name));
    sftp_attributes_free(attributes);
  }

  if (!sftp_dir_eof(dir)) {
    fprintf(stderr, "Can't list directory: %s\n", ssh_get_error(ssh));
    sftp_closedir(dir);
    return SSH_ERROR;
  }

  rc = sftp_closedir(dir);

  if (rc != SSH_OK) {
    fprintf(stderr, "Can't close directory: %s\n", ssh_get_error(ssh));
    return rc;
  }
  return rc;
}

struct Config {
  std::string sftp_host;
  std::string sftp_port;
  std::string sftp_user;
  std::string sftp_password;
  std::string sftp_remote_dir;
  std::string local_dir;
  std::string sql_user;
  std::string sql_password;
  std::string sql_database;
  int er = 0;

  Config(const std::string filename) {
    std::ifstream ifst(filename);

    if (!ifst.is_open()) {
      std::cout << "File does not open!\n";
      er = 1;
      return;
    }

    getline(ifst, sftp_host);
    getline(ifst, sftp_port);
    getline(ifst, sftp_user);
    getline(ifst, sftp_password);
    getline(ifst, sftp_remote_dir);
    getline(ifst, local_dir);
    getline(ifst, sql_user);
    getline(ifst, sql_password);
    getline(ifst, sql_database);

    size_t index = sftp_host.find_last_of('=') + 1;
    sftp_host = sftp_host.substr(index, sftp_host.size() - index);

    index = sftp_port.find_last_of('=') + 1;
    sftp_port = sftp_port.substr(index, sftp_port.size() - index);

    index = sftp_user.find_last_of('=') + 1;
    sftp_user = sftp_user.substr(index, sftp_user.size() - index);

    index = sftp_password.find_last_of('=') + 1;
    sftp_password = sftp_password.substr(index, sftp_password.size() - index);

    index = sftp_remote_dir.find_last_of('=') + 1;
    sftp_remote_dir =
        sftp_remote_dir.substr(index, sftp_remote_dir.size() - index);

    index = local_dir.find_last_of('=') + 1;
    local_dir = local_dir.substr(index, local_dir.size() - index);

    index = sql_user.find_last_of('=') + 1;
    sql_user = sql_user.substr(index, sql_user.size() - index);

    index = sql_password.find_last_of('=') + 1;
    sql_password = sql_password.substr(index, sql_password.size() - index);

    index = sql_database.find_last_of('=') + 1;
    sql_database = sql_database.substr(index, sql_database.size() - index);

    ifst.close();
  }
};

//................................................................

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cout << "Invalid parameters!" << std::endl;
    return 1;
  }

  Config conf(argv[1]);

  if (conf.er == 1) {
    return 1;
  }

  int port = stoi(conf.sftp_port);

  ssh_session ssh = ssh_new();

  ssh_options_set(ssh, SSH_OPTIONS_HOST, conf.sftp_host.c_str());
  ssh_options_set(ssh, SSH_OPTIONS_PORT, &port);

  ssh_connect(ssh);
  ssh_userauth_password(ssh, conf.sftp_user.c_str(),
                        conf.sftp_password.c_str());

  sftp_session sftp = sftp_new(ssh);

  if (sftp == NULL) {
    fprintf(stderr, "Error allocating SFTP session: %s\n", ssh_get_error(sftp));
    return SSH_ERROR;
  }

  int rc = sftp_init(sftp);

  if (rc != SSH_OK) {
    fprintf(stderr, "Error initializing SFTP session: %d.\n",
            sftp_get_error(sftp));
    sftp_free(sftp);
    return rc;
  }

  std::vector<std::string> vec;

  sftp_list_dir(ssh, sftp, vec, conf.sftp_remote_dir, conf.local_dir);

  int qstate;
  MYSQL* conn;
  MYSQL_ROW row;
  MYSQL_RES* res;

  conn = mysql_init(0);

  conn = mysql_real_connect(conn, "localhost", conf.sql_user.c_str(),
                            conf.sql_password.c_str(),
                            conf.sql_database.c_str(), 3306, NULL, 0);

  const std::string cr_db =
      "CREATE DATABASE IF NOT EXISTS " + conf.sql_database;

  qstate = mysql_query(conn, cr_db.c_str());

  if (qstate)
    std::cout << "Database cannot be created  " << mysql_error(conn)
              << std::endl;

  const std::string cr_tb_db =
      "CREATE TABLE IF NOT EXISTS " + conf.sql_database +
      ".`files` ( `id` INT(11) NOT NULL "
      "AUTO_INCREMENT , `filename` VARCHAR(60) NOT NULL, `date` DATETIME NOT "
      "NULL , PRIMARY KEY (`id`) ) ENGINE = InnoDB";

  qstate = mysql_query(conn, cr_tb_db.c_str());

  if (qstate)
    std::cout << "Table cannot be created  " << mysql_error(conn) << std::endl;

  const std::string use_db = "USE " + conf.sql_database;

  qstate = mysql_query(conn, use_db.c_str());

  if (qstate)
    std::cout << "Database cannot be used  " << mysql_error(conn) << std::endl;

  std::cout << std::endl;

  for (const std::string& var : vec) {
    std::string que =
        "INSERT INTO `files` (`id`, `filename`, `date`) VALUES (NULL, '" + var +
        "', NOW())";
    qstate = mysql_query(conn, que.c_str());
    if (qstate)
      std::cout << "Values cannot be inserted   " << mysql_error(conn)
                << std::endl;
  }

  if (!conn) {
    puts("Connection to database has failed!");
    sftp_free(sftp);
    ssh_disconnect(ssh);
    ssh_free(ssh);
    return 1;
  }

  std::string query = "SELECT * FROM `files`";

  qstate = mysql_query(conn, query.c_str());

  if (qstate) {
    std::cout << "Query failed: " << mysql_error(conn) << std::endl;
    sftp_free(sftp);
    ssh_disconnect(ssh);
    ssh_free(ssh);
    return 1;
  }
  res = mysql_store_result(conn);

  while (row = mysql_fetch_row(res)) {
    printf("File: %s %s \t Date: %s\n", row[0], row[1], row[2]);
  }

  sftp_free(sftp);
  ssh_disconnect(ssh);
  ssh_free(ssh);

  return 0;
}
package ProFTPD::Tests::Modules::mod_sql_cassandra;

use lib qw(t/lib);
use base qw(ProFTPD::TestSuite::Child);
use strict;

use Carp;
use File::Path qw(mkpath);
use File::Spec;
use IO::Handle;

use ProFTPD::TestSuite::FTP;
use ProFTPD::TestSuite::Utils qw(:auth :config :running :test :testsuite);

$| = 1;

my $order = 0;

my $TESTS = {
  sql_cassandra_login => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  sql_cassandra_sqllog => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  sql_cassandra_using_tls => {
    order => ++$order,
    test_class => [qw(forking inprogress)],
  },

};

sub new {
  return shift()->SUPER::new(@_);
}

sub list_tests {
  return testsuite_get_runnable_tests($TESTS);
}

sub get_cassandra_host {
  # This IP address is used by the my local Cassandra Vagrant VM
  my $cassandra_host = '192.168.21.21';

  if (defined($ENV{CASSANDRA_HOST})) {
    $cassandra_host = $ENV{CASSANDRA_HOST};
  }

  return $cassandra_host;
}

sub sql_cassandra_login {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'cassandra');

  my $cassandra_host = get_cassandra_host();

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'sql:20 sql.cassandra:20',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      'mod_sql.c' => [
        'SQLAuthTypes plaintext',
        'SQLBackend cassandra',
        "SQLConnectInfo proftpd\@$cassandra_host proftpd developer",
        "SQLLogFile $setup->{log_file}",
        'SQLMinID 0',

        # Required for current testing against a single node
        'SQLCassandraConsistency ONE',

        # Required
        'SQLAuthenticate users groups',

        'SQLNamedQuery get-user-by-name SELECT "userid, passwd, uid, gid, homedir, shell FROM users_by_name WHERE userid = \'%U\'"',
        'SQLNamedQuery get-user-by-id SELECT "userid, passwd, uid, gid, homedir, shell FROM users_by_id WHERE uid = %{0}"',
         'SQLNamedQuery get-user-names SELECT "userid FROM users_by_name"',
         'SQLNamedQuery get-all-users SELECT "userid, passwd, uid, gid, homedir, shell FROM users_by_name"',
        'SQLUserInfo custom:/get-user-by-name/get-user-by-id/get-user-names/get-all-users',

        'SQLNamedQuery get-group-by-name SELECT "groupname, gid, members FROM groups_by_name WHERE groupname = \'%{0}\'"',
        'SQLNamedQuery get-group-by-id SELECT "groupname, gid, members FROM groups_by_id WHERE gid = %{0}"',
        'SQLNamedQuery get-members-by-user SELECT "groupname, gid, members FROM groups_by_name WHERE members = \'%{0}\' ALLOW FILTERING"',
        'SQLGroupInfo custom:/get-group-by-name/get-group-by-id/get-members-by-user',
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      # Allow for server startup
      sleep(1);

      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_msgs = $client->response_msgs();
      my $nmsgs = scalar(@$resp_msgs);

      my $expected;

      $expected = 1;
      $self->assert($expected == $nmsgs,
        test_msg("Expected $expected, got $nmsgs"));

      $expected = "User proftpd logged in";
      $self->assert($expected eq $resp_msgs->[0],
        test_msg("Expected '$expected', got '$resp_msgs->[0]'"));

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  test_cleanup($setup->{log_file}, $ex);
}

sub sql_cassandra_sqllog {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'cassandra');

  my $cassandra_host = get_cassandra_host();

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'sql:20 sql.cassandra:20',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      'mod_sql.c' => [
        'SQLAuthTypes plaintext',
        'SQLBackend cassandra',
        "SQLConnectInfo proftpd\@$cassandra_host proftpd developer",
        "SQLLogFile $setup->{log_file}",
        'SQLMinID 0',

        # Required for current testing against a single node
        'SQLCassandraConsistency ONE',

        # Required
        'SQLAuthenticate users groups',

        'SQLNamedQuery get-user-by-name SELECT "userid, passwd, uid, gid, homedir, shell FROM users_by_name WHERE userid = \'%U\'"',
        'SQLNamedQuery get-user-by-id SELECT "userid, passwd, uid, gid, homedir, shell FROM users_by_id WHERE uid = %{0}"',
         'SQLNamedQuery get-user-names SELECT "userid FROM users_by_name"',
         'SQLNamedQuery get-all-users SELECT "userid, passwd, uid, gid, homedir, shell FROM users_by_name"',
        'SQLUserInfo custom:/get-user-by-name/get-user-by-id/get-user-names/get-all-users',

        'SQLNamedQuery get-group-by-name SELECT "groupname, gid, members FROM groups_by_name WHERE groupname = \'%{0}\'"',
        'SQLNamedQuery get-group-by-id SELECT "groupname, gid, members FROM groups_by_id WHERE gid = %{0}"',
        'SQLNamedQuery get-members-by-user SELECT "groupname, gid, members FROM groups_by_name WHERE members = \'%{0}\' ALLOW FILTERING"',
        'SQLGroupInfo custom:/get-group-by-name/get-group-by-id/get-members-by-user',

        'SQLNamedQuery session_start FREEFORM "INSERT INTO sessions (user, ip_addr, timestamp_ms) VALUES (\'%u\', \'%L\', %{epoch}%{millisecs})"',
        'SQLLog PASS session_start',
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      # Allow for server startup
      sleep(1);

      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_msgs = $client->response_msgs();
      my $nmsgs = scalar(@$resp_msgs);

      my $expected;

      $expected = 1;
      $self->assert($expected == $nmsgs,
        test_msg("Expected $expected, got $nmsgs"));

      $expected = "User proftpd logged in";
      $self->assert($expected eq $resp_msgs->[0],
        test_msg("Expected '$expected', got '$resp_msgs->[0]'"));

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  test_cleanup($setup->{log_file}, $ex);
}

sub sql_cassandra_using_tls {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'cassandra');

  my $ca_file = File::Spec->rel2abs('t/etc/modules/mod_sql_cassandra/cassandra.pem');

  my $cassandra_host = get_cassandra_host();

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'sql:20 sql.cassandra:20',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      'mod_sql.c' => [
        'SQLAuthTypes plaintext',
        'SQLBackend cassandra',
        "SQLConnectInfo proftpd\@$cassandra_host proftpd developer ssl-ca:$ca_file",
        "SQLLogFile $setup->{log_file}",
        'SQLMinID 0',

        # Required for current testing against a single node
        'SQLCassandraConsistency ONE',

        # Required
        'SQLAuthenticate users groups',

        'SQLNamedQuery get-user-by-name SELECT "userid, passwd, uid, gid, homedir, shell FROM users_by_name WHERE userid = \'%U\'"',
        'SQLNamedQuery get-user-by-id SELECT "userid, passwd, uid, gid, homedir, shell FROM users_by_id WHERE uid = %{0}"',
         'SQLNamedQuery get-user-names SELECT "userid FROM users_by_name"',
         'SQLNamedQuery get-all-users SELECT "userid, passwd, uid, gid, homedir, shell FROM users_by_name"',
        'SQLUserInfo custom:/get-user-by-name/get-user-by-id/get-user-names/get-all-users',

        'SQLNamedQuery get-group-by-name SELECT "groupname, gid, members FROM groups_by_name WHERE groupname = \'%{0}\'"',
        'SQLNamedQuery get-group-by-id SELECT "groupname, gid, members FROM groups_by_id WHERE gid = %{0}"',
        'SQLNamedQuery get-members-by-user SELECT "groupname, gid, members FROM groups_by_name WHERE members = \'%{0}\' ALLOW FILTERING"',
        'SQLGroupInfo custom:/get-group-by-name/get-group-by-id/get-members-by-user',
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      # Allow for server startup
      sleep(1);

      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_msgs = $client->response_msgs();
      my $nmsgs = scalar(@$resp_msgs);

      my $expected;

      $expected = 1;
      $self->assert($expected == $nmsgs,
        test_msg("Expected $expected, got $nmsgs"));

      $expected = "User proftpd logged in";
      $self->assert($expected eq $resp_msgs->[0],
        test_msg("Expected '$expected', got '$resp_msgs->[0]'"));

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  test_cleanup($setup->{log_file}, $ex);
}

1;

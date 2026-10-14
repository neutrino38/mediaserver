/**
 * speech-server: ASR, TTS and text completion by Mistral AI, served over
 * WebSocket (docs/conception/SPEECH-SERVER/SPEC.md).
 */
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include "log.h"
#include "version.h"
#include "websocketserver.h"
#include "xmlrpcserver.h"
#include "speech/controlhandler.h"
#include "speech/speechconfig.h"
#include "speech/speechstatus.h"
#include "speech/streaminghandler.h"
#include "speech/tokenstore.h"
#include "speech/usage.h"

namespace
{

const char* DefaultConfigPath = "/etc/mediaserver/speech-config.json";

// The key file must be readable by its owner only (SPEC §8.1).
bool ReadApiKey(const std::string& path, std::string& key, std::string& error)
{
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
	{
		error = "cannot find the Mistral key " + path;
		return false;
	}
	if (st.st_mode & (S_IRWXG | S_IRWXO))
	{
		error = "the Mistral key " + path + " is readable by others: chmod 0600";
		return false;
	}

	std::ifstream file(path);
	std::stringstream text;
	text << file.rdbuf();
	key = text.str();
	key.erase(key.find_last_not_of(" \t\r\n") + 1);
	key.erase(0, key.find_first_not_of(" \t\r\n"));
	if (!file || key.empty())
	{
		error = "the Mistral key " + path + " is empty or unreadable";
		return false;
	}
	return true;
}

void Usage(const char* name)
{
	printf("Usage: %s [--config <path>] [--api-key-file <path>] [--check]\n", name);
	printf("  --config <path>        default %s\n", DefaultConfigPath);
	printf("  --api-key-file <path>  default $CREDENTIALS_DIRECTORY/mistral-api-key\n");
	printf("  --check                validate the configuration and the key, then exit\n");
}

}

int main(int argc, char** argv)
{
	const time_t startedAt = time(NULL);

	std::string configPath = DefaultConfigPath;
	std::string keyPath;
	bool check = false;

	static const option options[] = {
		{"config",       required_argument, nullptr, 'c'},
		{"api-key-file", required_argument, nullptr, 'k'},
		{"check",        no_argument,       nullptr, 'C'},
		{"help",         no_argument,       nullptr, 'h'},
		{nullptr, 0, nullptr, 0}};

	int opt;
	while ((opt = getopt_long(argc, argv, "", options, nullptr)) != -1)
	{
		switch (opt)
		{
			case 'c': configPath = optarg; break;
			case 'k': keyPath = optarg; break;
			case 'C': check = true; break;
			case 'h': Usage(argv[0]); return 0;
			default:  Usage(argv[0]); return 2;
		}
	}

	if (keyPath.empty())
	{
		const char* credentials = getenv("CREDENTIALS_DIRECTORY");
		if (credentials)
			keyPath = std::string(credentials) + "/mistral-api-key";
	}

	Log("-speech-server %s starting [config:%s]\n", MCUVERSION, configPath.c_str());

	std::string error;
	std::shared_ptr<const SpeechConfig> loaded = SpeechConfig::Load(configPath, error);
	if (!loaded)
	{
		Error("-speech-server: %s\n", error.c_str());
		return 1;
	}

	std::string apiKey;
	if (keyPath.empty())
	{
		Error("-speech-server: no Mistral key: run under systemd LoadCredential, or pass --api-key-file\n");
		return 1;
	}
	if (!ReadApiKey(keyPath, apiKey, error))
	{
		Error("-speech-server: %s\n", error.c_str());
		return 1;
	}

	if (check)
	{
		printf("configuration %s: OK\n", configPath.c_str());
		printf("Mistral key: present\n");
		printf("control:   %s port %d\n", loaded->controlListen.ToString().c_str(), loaded->controlPort);
		printf("streaming: %s port %d (%s)\n", loaded->streamingListen.ToString().c_str(), loaded->streamingPort,
		       loaded->streamingSecure ? "wss" : "ws");
		printf("voices:    %zu\n", loaded->voices.size());
		return 0;
	}

	// Signals are taken by one thread, with sigwait: the reload and the
	// shutdown run as plain code, never inside a signal handler. Blocked
	// before any thread starts, so that every thread inherits the mask.
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGHUP);
	sigaddset(&signals, SIGTERM);
	sigaddset(&signals, SIGINT);
	pthread_sigmask(SIG_BLOCK, &signals, nullptr);
	signal(SIGPIPE, SIG_IGN);

	SpeechConfigHolder config(loaded);
	TokenStore tokens;
	UsageCounters usage;

	StreamingHandler streaming(config, tokens);
	WebSocketServer wsServer;
	wsServer.SetSecure(loaded->streamingSecure, loaded->streamingCert.c_str(), loaded->streamingKey.c_str());
	wsServer.AddHandler("/streaming/", &streaming);

	SpeechStatus status(startedAt, true, streaming, tokens, usage);
	ControlHandler control(config, tokens, usage, status);
	XmlRpcServer controlServer(loaded->controlListen.To(loaded->controlPort));
	controlServer.AddHandler("/", &control);

	if (!wsServer.Init(loaded->streamingListen.To(loaded->streamingPort)))
	{
		Error("-speech-server: cannot listen on the streaming port %d\n", loaded->streamingPort);
		return 1;
	}
	streaming.Start();

	std::thread signalThread([&]() {
		for (;;)
		{
			int sig = 0;
			if (sigwait(&signals, &sig) != 0)
				continue;

			if (sig == SIGHUP)
			{
				std::string reloadError;
				std::shared_ptr<const SpeechConfig> next = SpeechConfig::Load(configPath, reloadError);
				if (!next)
				{
					Error("-speech-server: reload ignored, the current configuration stays: %s\n", reloadError.c_str());
					continue;
				}
				if (config.Get()->NeedsRestartToApply(*next))
					Log("-speech-server: ports, listen addresses, TLS and mistral.* change only on restart\n");
				config.Set(next);
				Log("-speech-server: configuration reloaded\n");
				continue;
			}

			Log("-speech-server: stopping\n");
			streaming.CloseAll(SpeechSession::CloseGoingAway, "going away", 2000);
			controlServer.Stop();
			return;
		}
	});

	// Blocks until Stop(); returns 0 at once when the port cannot be bound.
	const int served = controlServer.Start();
	if (!served)
	{
		Error("-speech-server: cannot listen on the control port %d\n", loaded->controlPort);
		pthread_kill(signalThread.native_handle(), SIGTERM);
	}
	signalThread.join();

	streaming.Stop();
	wsServer.End();

	Log("-speech-server stopped\n");
	return served ? 0 : 1;
}

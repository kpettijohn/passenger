/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2010-2013 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */

module.paths.unshift(__dirname + "/../node_lib");
var EventEmitter = require('events').EventEmitter;
var net = require('net');
var http = require('http');

var LineReader = require('phusion_passenger/line_reader').LineReader;

module.isApplicationLoader = true; // https://groups.google.com/forum/#!topic/compoundjs/4txxkNtROQg
GLOBAL.PhusionPassenger = exports.PhusionPassenger = new EventEmitter();
var stdinReader = new LineReader(process.stdin);
beginHandshake();
readInitializationHeader();


function beginHandshake() {
	process.stdout.write("!> I have control 1.0\n");
}

function readInitializationHeader() {
	stdinReader.readLine(function(line) {
		if (line != "You have control 1.0\n") {
			console.error('Invalid initialization header');
			process.exit(1);
		} else {
			readOptions();
		}
	});
}

function readOptions() {
	var options = {};

	function readNextOption() {
		stdinReader.readLine(function(line) {
			if (line == "\n") {
				setupEnvironment(options);
			} else if (line == "") {
				console.error("End of stream encountered while reading initialization options");
				process.exit(1);
			} else {
				var matches = line.replace(/\n/, '').match(/(.*?) *: *(.*)/);
				options[matches[1]] = matches[2];
				readNextOption();
			}
		});
	}

	readNextOption();
}

function setupEnvironment(options) {
	PhusionPassenger.options = options;
	PhusionPassenger.configure = configure;
	PhusionPassenger._appInstalled = false;
	process.title = 'Passenger NodeApp: ' + options.app_root;
	http.Server.prototype.originalListen = http.Server.prototype.listen;
	http.Server.prototype.listen = installServer;

	stdinReader.close();
	stdinReader = undefined;
	process.stdin.on('end', shutdown);
	process.stdin.resume();

	loadApplication();
}

/**
 * PhusionPassenger.configure(options)
 *
 * Configures Phusion Passenger's behavior inside this Node application.
 *
 * Options:
 *   autoInstall (boolean, default true)
 *     Whether to install the first HttpServer object for which listen() is called,
 *     as the Phusion Passenger request handler.
 */
function configure(_options) {
	var options = {
		autoInstall: true
	};
	for (var key in _options) {
		options[key] = _options[key];
	}

    if (!options.autoInstall) {
		http.Server.prototype.listen = listenAndMaybeInstall;
	}
}

function loadApplication() {
	var startupFile = PhusionPassenger.options.startup_file || 'app.js';
	require(PhusionPassenger.options.app_root + '/' + startupFile);
}

function extractCallback(args) {
	if (args.length > 1 && typeof(args[args.length - 1]) == 'function') {
		return args[args.length - 1];
	}
}

function generateServerSocketPath() {
	return PhusionPassenger.options.generation_dir + "/backends/"
		+ process.pid + "." + ((Math.random() * 0xFFFFFFFF) & 0xFFFFFFF);
}

function addListenerAtBeginning(emitter, event, callback) {
	var listeners = emitter.listeners(event);
	var i;

	emitter.removeAllListeners(event);
	emitter.on(event, callback);
	for (i = 0; i < listeners.length; i++) {
		emitter.on(event, listeners[i]);
	}
}

function installServer() {
	var server = this;
	if (!PhusionPassenger._appInstalled) {
		PhusionPassenger._appInstalled = true;
		PhusionPassenger._server = server;

		// Ensure that req.connection.remoteAddress and remotePort return something
		// instead of undefined. Apps like Etherpad expect it.
		// See https://github.com/phusion/passenger/issues/1224
		addListenerAtBeginning(server, 'request', function(req) {
			req.connection.__defineGetter__('remoteAddress', function() {
				return '127.0.0.1';
			});
			req.connection.__defineGetter__('remotePort', function() {
				return 0;
			});
		});

		var listenTries = 0;
		doListen(extractCallback(arguments));

		function doListen(callback) {
			function errorHandler(error) {
				if (error.errno == 'EADDRINUSE') {
					if (listenTries == 100) {
						server.emit('error', new Error(
							'Phusion Passenger could not find suitable socket address to bind on'));
					} else {
						// Try again with another socket path.
						listenTries++;
						doListen(callback);
					}
				} else {
					server.emit('error', error);
				}
			}

			server.once('error', errorHandler);
			server.originalListen(generateServerSocketPath(), function() {
				server.removeListener('error', errorHandler);
				doneListening(callback);
				process.nextTick(finalizeStartup);
			});
		}

		function doneListening(callback) {
			if (callback) {
				server.once('listening', callback);
			}
			server.emit('listening');
		}

		return server;
	} else {
		throw new Error("http.Server.listen() was called more than once, which " +
			"is not allowed because Phusion Passenger is in auto-install mode. " +
			"This means that the first http.Server object for which listen() is called, " +
			"is automatically installed as the Phusion Passenger request handler. " +
			"If you want to create and listen on multiple http.Server object then " +
			"you should disable auto-install mode. Please read " +
			"http://stackoverflow.com/questions/20645231/phusion-passenger-error-http-server-listen-was-called-more-than-once/20645549");
	}
}

function listenAndMaybeInstall(port) {
	if (port === 'passenger') {
		if (!PhusionPassenger._appInstalled) {
			return installServer.apply(this, arguments);
		} else {
			throw new Error("You may only call listen('passenger') once. Please read http://stackoverflow.com/questions/20645231/phusion-passenger-error-http-server-listen-was-called-more-than-once/20645549");
		}
	} else {
		return this.originalListen.apply(this, arguments);
	}
}

function finalizeStartup() {
	process.stdout.write("!> Ready\n");
	process.stdout.write("!> socket: main;unix:" +
		PhusionPassenger._server.address() +
		";http_session;0\n");
	process.stdout.write("!> \n");
}

function shutdown() {
	if (PhusionPassenger.listeners('exit').length == 0) {
		process.exit(0);
	} else {
		PhusionPassenger.emit('exit');
	}
}

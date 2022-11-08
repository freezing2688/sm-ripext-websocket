/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod REST in Pawn Extension
 * Copyright 2017-2022 Erik Minekus
 * =============================================================================
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "extension.h"
#include "httprequest.h"
#include "queue.h"
#include "websocket_connection_base.h"
#include "event_loop.h"
#include <atomic>

// Limit the max processing request per tick
#define MAX_PROCESS 10

RipExt g_RipExt; /**< Global singleton for extension's main interface */

SMEXT_LINK(&g_RipExt);

LockedQueue<IHTTPContext *> g_RequestQueue;
LockedQueue<IHTTPContext *> g_CompletedRequestQueue;

CURLM *g_Curl;
uv_loop_t *g_Loop;
uv_thread_t g_Thread;
uv_timer_t g_Timeout;

uv_async_t g_AsyncPerformRequests;
uv_async_t g_AsyncStopLoop;

HTTPRequestHandler g_HTTPRequestHandler;
HandleType_t htHTTPRequest;

HTTPResponseHandler g_HTTPResponseHandler;
HandleType_t htHTTPResponse;

JSONHandler g_JSONHandler;
HandleType_t htJSON;

JSONObjectKeysHandler g_JSONObjectKeysHandler;
HandleType_t htJSONObjectKeys;

WebSocketHandler g_WebSocketHandler;
HandleType_t htWebSocket;

std::atomic<bool> unloaded;

static void CheckCompletedRequests()
{
	CURLMsg *message;
	int pending;

	while ((message = curl_multi_info_read(g_Curl, &pending)))
	{
		if (message->msg != CURLMSG_DONE)
		{
			continue;
		}

		CURL *curl = message->easy_handle;
		curl_multi_remove_handle(g_Curl, curl);

		IHTTPContext *context;
		curl_easy_getinfo(curl, CURLINFO_PRIVATE, &context);

		g_CompletedRequestQueue.Lock();
		g_CompletedRequestQueue.Push(context);
		g_CompletedRequestQueue.Unlock();
	}
}

static void PerformRequests(uv_timer_t *handle)
{
	int running;
	curl_multi_socket_action(g_Curl, CURL_SOCKET_TIMEOUT, 0, &running);

	CheckCompletedRequests();
}

static void CurlSocketActivity(uv_poll_t *handle, int status, int events)
{
	CurlContext *context = (CurlContext *)handle->data;
	int flags = 0;

	if (events & UV_READABLE)
	{
		flags |= CURL_CSELECT_IN;
	}
	if (events & UV_WRITABLE)
	{
		flags |= CURL_CSELECT_OUT;
	}

	int running;
	curl_multi_socket_action(g_Curl, context->socket, flags, &running);

	CheckCompletedRequests();
}

static int CurlSocketCallback(CURL *curl, curl_socket_t socket, int action, void *userdata, void *socketdata)
{
	CurlContext *context;
	int events = 0;

	switch (action)
	{
	case CURL_POLL_IN:
	case CURL_POLL_OUT:
	case CURL_POLL_INOUT:
		context = socketdata ? (CurlContext *)socketdata : new CurlContext(socket);
		curl_multi_assign(g_Curl, socket, context);

		if (action != CURL_POLL_IN)
		{
			events |= UV_WRITABLE;
		}
		if (action != CURL_POLL_OUT)
		{
			events |= UV_READABLE;
		}

		uv_poll_start(&context->poll_handle, events, &CurlSocketActivity);
		break;
	case CURL_POLL_REMOVE:
		if (socketdata)
		{
			context = (CurlContext *)socketdata;
			context->Destroy();

			curl_multi_assign(g_Curl, socket, nullptr);
		}
		break;
	}

	return 0;
}

static int CurlTimeoutCallback(CURLM *multi, long timeout_ms, void *userdata)
{
	if (timeout_ms == -1)
	{
		uv_timer_stop(&g_Timeout);
		return 0;
	}

	uv_timer_start(&g_Timeout, &PerformRequests, timeout_ms, 0);
	return 0;
}

static void EventLoop(void *data)
{
	uv_run(g_Loop, UV_RUN_DEFAULT);
}

static void AsyncPerformRequests(uv_async_t *handle)
{
	g_RequestQueue.Lock();
	IHTTPContext *context;
	// Limiter
	int count = 0;

	while (!g_RequestQueue.Empty() && count < MAX_PROCESS)
	{
		context = g_RequestQueue.Pop();

		if (!context->InitCurl())
		{
			delete context;
			continue;
		}

		curl_multi_add_handle(g_Curl, context->curl);
		count++;
	}

	g_RequestQueue.Unlock();
}

static void AsyncStopLoop(uv_async_t *handle)
{
	uv_stop(g_Loop);
}

static void FrameHook(bool simulating)
{
	if (!g_RequestQueue.Empty())
	{
		uv_async_send(&g_AsyncPerformRequests);
	}

	if (!g_CompletedRequestQueue.Empty())
	{
		g_CompletedRequestQueue.Lock();
		IHTTPContext *context = g_CompletedRequestQueue.Pop();

		context->OnCompleted();
		delete context;

		g_CompletedRequestQueue.Unlock();
	}
}

bool RipExt::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	sharesys->AddNatives(myself, http_natives);
	sharesys->AddNatives(myself, json_natives);
	sharesys->AddNatives(myself, websocket_natives);
	sharesys->RegisterLibrary(myself, "ripext");

	/* Initialize cURL */
	CURLcode res = curl_global_init(CURL_GLOBAL_ALL);
	if (res != CURLE_OK)
	{
		smutils->Format(error, maxlength, "%s", curl_easy_strerror(res));
		return false;
	}

	g_Curl = curl_multi_init();
	curl_multi_setopt(g_Curl, CURLMOPT_SOCKETFUNCTION, &CurlSocketCallback);
	curl_multi_setopt(g_Curl, CURLMOPT_TIMERFUNCTION, &CurlTimeoutCallback);

	/* Initialize libuv */
	g_Loop = uv_default_loop();
	uv_timer_init(g_Loop, &g_Timeout);
	uv_async_init(g_Loop, &g_AsyncPerformRequests, &AsyncPerformRequests);
	uv_async_init(g_Loop, &g_AsyncStopLoop, &AsyncStopLoop);
	uv_thread_create(&g_Thread, &EventLoop, nullptr);

	/* Set up access rights for the 'HTTPRequest' handle type */
	HandleAccess haHTTPRequest;
	handlesys->InitAccessDefaults(nullptr, &haHTTPRequest);
	haHTTPRequest.access[HandleAccess_Delete] = 0;

	/* Set up access rights for the 'HTTPResponse' handle type */
	HandleAccess haHTTPResponse;
	handlesys->InitAccessDefaults(nullptr, &haHTTPResponse);
	haHTTPResponse.access[HandleAccess_Clone] = HANDLE_RESTRICT_IDENTITY;

	/* Set up access rights for the 'JSON' handle type */
	HandleAccess haJSON;
	handlesys->InitAccessDefaults(nullptr, &haJSON);
	haJSON.access[HandleAccess_Delete] = 0;

	/* Set up access rights for the 'WebSocket' handle type */
	HandleAccess haWS;
	TypeAccess taWS;

	handlesys->InitAccessDefaults(&taWS, &haWS);
	taWS.ident = myself->GetIdentity();
	haWS.access[HandleAccess_Read] = HANDLE_RESTRICT_OWNER;
	taWS.access[HTypeAccess_Create] = true;
	taWS.access[HTypeAccess_Inherit] = true;

	htHTTPRequest = handlesys->CreateType("HTTPRequest", &g_HTTPRequestHandler, 0, nullptr, &haHTTPRequest, myself->GetIdentity(), nullptr);
	htHTTPResponse = handlesys->CreateType("HTTPResponse", &g_HTTPResponseHandler, 0, nullptr, &haHTTPResponse, myself->GetIdentity(), nullptr);
	htJSON = handlesys->CreateType("JSON", &g_JSONHandler, 0, nullptr, &haJSON, myself->GetIdentity(), nullptr);
	htJSONObjectKeys = handlesys->CreateType("JSONObjectKeys", &g_JSONObjectKeysHandler, 0, nullptr, nullptr, myself->GetIdentity(), nullptr);
	htWebSocket = handlesys->CreateType("WebSocket", &g_WebSocketHandler, 0, &taWS, &haWS, myself->GetIdentity(), nullptr);

	smutils->AddGameFrameHook(&FrameHook);
	smutils->BuildPath(Path_SM, caBundlePath, sizeof(caBundlePath), SM_RIPEXT_CA_BUNDLE_PATH);

	event_loop.OnExtLoad();

	unloaded.store(false);

	return true;
}

void RipExt::SDK_OnUnload()
{
	uv_async_send(&g_AsyncStopLoop);
	uv_thread_join(&g_Thread);
	uv_loop_close(g_Loop);

	curl_multi_cleanup(&g_Curl);
	curl_global_cleanup();

	handlesys->RemoveType(htHTTPRequest, myself->GetIdentity());
	handlesys->RemoveType(htHTTPResponse, myself->GetIdentity());
	handlesys->RemoveType(htJSON, myself->GetIdentity());
	handlesys->RemoveType(htJSONObjectKeys, myself->GetIdentity());
	handlesys->RemoveType(htWebSocket, myself->GetIdentity());

	smutils->RemoveGameFrameHook(&FrameHook);

	event_loop.OnExtUnload();

	unloaded.store(true);
}

void RipExt::AddRequestToQueue(IHTTPContext *context)
{
	g_RequestQueue.Lock();
	g_RequestQueue.Push(context);
	g_RequestQueue.Unlock();
}

void log_msg(void *msg)
{
	if (!unloaded.load())
	{
		smutils->LogMessage(myself, reinterpret_cast<char *>(msg));
	}
	free(msg);
}

void log_err(void *msg)
{
	if (!unloaded.load())
	{
		smutils->LogError(myself, reinterpret_cast<char *>(msg));
	}
	free(msg);
}

void RipExt::LogMessage(const char *msg, ...)
{
	char *buffer = reinterpret_cast<char *>(malloc(3072));
	va_list vp;
	va_start(vp, msg);
	vsnprintf(buffer, 3072, msg, vp);
	va_end(vp);

	smutils->AddFrameAction(&log_msg, reinterpret_cast<void *>(buffer));
}

void RipExt::LogError(const char *msg, ...)
{
	char *buffer = reinterpret_cast<char *>(malloc(3072));
	va_list vp;
	va_start(vp, msg);
	vsnprintf(buffer, 3072, msg, vp);
	va_end(vp);

	smutils->AddFrameAction(&log_err, reinterpret_cast<void *>(buffer));
}

void execute_cb(void *cb)
{
	std::unique_ptr<std::function<void()>> callback(reinterpret_cast<std::function<void()> *>(cb));
	callback->operator()();
}

void RipExt::Defer(std::function<void()> callback)
{
	std::unique_ptr<std::function<void()>> cb = std::make_unique<std::function<void()>>(callback);
	smutils->AddFrameAction(&execute_cb, cb.release());
}

void HTTPRequestHandler::OnHandleDestroy(HandleType_t type, void *object)
{
	delete (HTTPRequest *)object;
}

void HTTPResponseHandler::OnHandleDestroy(HandleType_t type, void *object)
{
	/* Response objects are automatically cleaned up */
}

void JSONHandler::OnHandleDestroy(HandleType_t type, void *object)
{
	json_decref((json_t *)object);
}

void JSONObjectKeysHandler::OnHandleDestroy(HandleType_t type, void *object)
{
	delete (struct JSONObjectKeys *)object;
}

void WebSocketHandler::OnHandleDestroy(HandleType_t type, void *object)
{
	reinterpret_cast<websocket_connection_base *>(object)->destroy();
}

bool WebSocketHandler::GetHandleApproxSize(HandleType_t type, void *object, unsigned int *size)
{
	*size = sizeof(websocket_connection_base);
	return true;
}

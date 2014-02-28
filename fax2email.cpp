/**
 * fax2email.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Fax2email module
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2006 Null Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <yatephone.h>
#include <time.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

using namespace TelEngine;
namespace { // anonymous

/*
** Translation Table as described in RFC1113
*/
static const char cb64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

class Fax2EmailRec : public NamedString
{
public:
    Fax2EmailRec(const char* name, const char* value, const char* email, const char* fromNumber, RefObject* userData)
        : NamedString(name, value), m_email(email), m_from(fromNumber), m_userData(userData) {
        if (m_userData)
            m_userData->ref();
    }
    virtual ~Fax2EmailRec() {
        if (m_userData)
            m_userData->deref();
    }

    virtual void* getObject(const String& name) const {
	if (name == "Fax2EmailRec")
  	    return (void*)this;
	return NamedString::getObject(name);
    }

    inline String& getEmail() {
	return m_email;
    }

    inline String& getFrom() {
	return m_from;
    }
    
    inline RefObject* getUserData() {
        return m_userData;
    }
	
private:
    String m_email;
    String m_from;
    RefObject* m_userData;
};

class FaxLimit : public String
{
public:
    FaxLimit(const char* value, int len = -1)
      : String(value, len), m_counter(0) {};
    FaxLimit(const String &value)
      : String(value), m_counter(0) {};
    inline int ref() { return ++m_counter; };
    inline int unref() { return --m_counter; };
    virtual void* getObject(const String& name) const {
	if (name == "FaxLimit")
  	    return (void*)this;
	return String::getObject(name);
    }
private:
    int m_counter;
};

class Fax2EmailModule : public Module
{
public:
    enum {
        CallRoute = Private,
        ChanHangup = (Private << 1)
    };
    Fax2EmailModule();
    ~Fax2EmailModule();
    void send_email(const char* to, const char* from, const char* subject, const char* body, const char* attach);
    bool unload();
    virtual void initialize();
    virtual bool received(Message& msg, int id);
    bool msgRoute(Message& msg);
    bool msgHangup(Message& msg);
private:
    bool m_init;
    HashList m_hash;
    HashList m_limit;
    String m_account;
    String m_emailFrom;
    int m_chanHangupPrio;
    int m_callRoutePrio;
};

/*
** encodeblock
**
** encode 3 8-bit binary bytes as 4 '6-bit' characters
*/
void encodeblock(unsigned char in[3], unsigned char out[4], int len)
{
    out[0] = cb64[ in[0] >> 2 ];
    out[1] = cb64[ ((in[0] & 0x03) << 4) | ((in[1] & 0xf0) >> 4) ];
    out[2] = (unsigned char) (len > 1 ? cb64[ ((in[1] & 0x0f) << 2) | ((in[2] & 0xc0) >> 6) ] : '=');
    out[3] = (unsigned char) (len > 2 ? cb64[ in[2] & 0x3f ] : '=');
}

String encodeString(const String input)
{
    char in[3], out[4];
    unsigned int i;
    String res;
    i = 0;
    while (i < input.length()) {
	in[i % 3] = input[i];
	if ((++i) % 3 == 0) {
	    encodeblock(reinterpret_cast<unsigned char*>(in), reinterpret_cast<unsigned char*>(out), 3);
	    for (int n = 0; n<4; n++)
		res << out[n];
	}
    }
    if (i % 3) {
	encodeblock(reinterpret_cast<unsigned char*>(in), reinterpret_cast<unsigned char*>(out), i % 3);
	for (int n = 0; n<4; n++)
	    res << out[n];
    }
    return res;
}

String encodeData(unsigned char* input, int len)
{
    unsigned char in[3];
    char out[4];
    int i, line_len;
    String res;
    i = 0;
    line_len = 0;
    while (i < len) {
	in[i % 3] = input[i];
	if ((++i) % 3 == 0) {
	    encodeblock(in, reinterpret_cast<unsigned char*>(out), 3);
	    for (int n = 0; n<4; n++)
		res << out[n];
	    line_len += 4;
	    if (line_len >= 80) {
		res << "\n";
		line_len = 0;
	    }
	}
    }
    if (i % 3) {
	encodeblock(in, reinterpret_cast<unsigned char*>(out), i % 3);
	for (int n = 0; n<4; n++)
	    res << out[n];
    }
    return res;
}




// copy parameters from SQL result to a NamedList
static void copyParams(NamedList& lst, Array* a)
{
    if (!a)
        return;
    for (int i = 0; i < a->getColumns(); i++) {
        String* s = YOBJECT(String,a->get(i,0));
        if (!(s && *s))
            continue;
        String name = *s;
        for (int j = 1; j < a->getRows(); j++) {
            s = YOBJECT(String,a->get(i,j));
            if (s)
                lst.setParam(name,*s);
        }
    }
}

INIT_PLUGIN(Fax2EmailModule);

UNLOAD_PLUGIN(unloadNow)
{
    if (unloadNow && !__plugin.unload())
        return false;
    return true;
}

void Fax2EmailModule::send_email(const char* to, const char* from, const char* subject, const char* body, const char* attach)
{
    char* cboundary = (char*)malloc(255);
    snprintf(cboundary, 255, "%d%d", time(NULL), time(NULL));
    String boundary = encodeString(String(cboundary));
    free(cboundary);

    char* tmp = (char*)malloc(1024);
    snprintf(tmp, 1024, "%s.letter", attach);
    FILE* handle = popen("sendmail -ti", "w");
    //FILE* handle1 = fopen(tmp, "w");

    snprintf(tmp, 1024, "To: %s\nFrom: %s\nSubject: %s\nMIME-Version: 1.0\nContent-Type: multipart/mixed; boundary=\"%s\"\nContent-Disposition: inline\n\n",
	to, from, subject, boundary.c_str());
    fwrite(tmp, strlen(tmp), 1, handle);
    //fwrite(tmp, strlen(tmp), 1, handle1);
    
    snprintf(tmp, 1024, "\n--%s\nContent-Type: text/plain; charset=us-ascii\nContent-Disposition: inline\n\n", boundary.c_str());
    fwrite(tmp, strlen(tmp), 1, handle);
    fwrite(body, 1, strlen(body), handle);
    //fwrite(tmp, strlen(tmp), 1, handle1);
    //fwrite(body, 1, strlen(body), handle1);

    time_t t;
    struct tm *tim;
	
    t = time(NULL);
    tim = localtime(&t);
    char* prefix = (char*)malloc(255);
    if (!strftime(prefix, 255, "fax%Y-%m-%d_%H-%M-%S", tim))
	prefix = "Error-in-strftime";

    //char* filename = (char*)malloc(1024);
    snprintf(tmp, 1024, "/usr/bin/tiff2pdf -o %1$s.pdf %1$s", attach);
    Debug(&__plugin, DebugInfo, "Running: %s", tmp);
    /*int status = system(tmp);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
	Debug(&__plugin, DebugInfo, "Conversion to PDF Succeed! %d", status);
	snprintf(filename, 1024, "%s.pdf", attach);
	snprintf(tmp, 1024, "\n--%s\nContent-Type: application/pdf\nContent-Disposition: attachment; filename=\"%s.pdf\"\nContent-Transfer-Encoding: base64\n\n", 
	    boundary.c_str(), prefix);
    } else {
        Debug(&__plugin, DebugInfo, "Conversion to PDF Failed! %d", status);
	snprintf(filename, 1024, "%s", attach);
	snprintf(tmp, 1024, "\n--%s\nContent-Type: image/tiff\nContent-Disposition: attachment; filename=\"%s.tiff\"\nContent-Transfer-Encoding: base64\n\n", 
            boundary.c_str(), prefix);
    }/**/

    snprintf(tmp, 1024, "\n--%s\nContent-Type: application/pdf\nContent-Disposition: attachment; filename=\"%s.pdf\"\nContent-Transfer-Encoding: base64\n\n", 
        boundary.c_str(), prefix);
    fwrite(tmp, strlen(tmp), 1, handle);
    //fwrite(tmp, strlen(tmp), 1, handle1);
    free(prefix);

    snprintf(tmp, 1024, "/usr/bin/tiff2pdf %s", attach);
    Debug(&__plugin, DebugInfo, "Running: %s", tmp);
    FILE* attach_file = popen(tmp, "r");
    //FILE* attach_file = fopen(filename, "r");
    size_t read_size;
    String attach_body;
    unsigned char buf[1023];
    while (1023 == (read_size = fread(buf, 1, 1023, attach_file))) {
	attach_body << encodeData(buf, read_size);
    }
    if (read_size)
	attach_body << encodeData(buf, read_size);
    pclose(attach_file);
    //fclose(attach_file);
    fwrite(attach_body.c_str(), 1, attach_body.length(), handle);
    
    snprintf(tmp, 1024, "\n\n--%s--", boundary.c_str());
    fwrite(tmp, strlen(tmp), 1, handle);
    //fwrite(tmp, strlen(tmp), 1, handle1);
    free(tmp);
    
    pclose(handle);
    //fclose(handle1);
    
    //unlink(filename);
    //free(filename);
}

bool Fax2EmailModule::unload()
{
    if (!lock(500000))
	return false;
    uninstallRelays();
    unlock();
    return true;
}

bool Fax2EmailModule::msgRoute(Message& msg)
{
    const char* called = msg.getValue("called");
    Message db("database");
    String query = "SELECT * FROM fax2email WHERE number = '";
    query += called;
    query += "'";
    db.addParam("query", query);
    db.addParam("account", m_account);
    if (!Engine::dispatch(db) || db.getIntValue("rows") < 1) {
	const char* error = db.getValue("error","failure");
	Debug(&__plugin, DebugWarn, "Could not fetch db data. Error:  '%s'", error);
	return false;
    }
    Array *result = static_cast<Array*>(db.userObject("Array"));
    if (!result) {
	Debug(&__plugin, DebugWarn, "Result array is NULL");
	return false;
    }
    
    if (result->getRows() <= 1 || result->getColumns() < 2) {
	Debug(&__plugin, DebugInfo, "Result array is empty");
	return false;
    }

    NamedList lst("templist");
    copyParams(lst, result);
    String dbg;
    lst.dump(dbg, ":", '"', true);

    //String email = result->get(1,1)->toString();
    Lock lock(this);
    int limit = result->get(2,1)->toString().toInteger(1);
    GenObject * ptr = m_limit[called];
    FaxLimit* limObj = 0;
    if (ptr) 
      limObj = static_cast<FaxLimit*>(ptr->getObject("FaxLimit"));
    if (!limObj) {
	limObj = new FaxLimit(called);
	m_limit.append(limObj);
    }
    if (limObj->ref() > limit) {
	if (limObj->unref() <= 0)
	    m_limit.remove(limObj, true);
	Debug(&__plugin, DebugMild, "Rejected call %s to %s to fax (%s): limit of %d calls exceeded", msg.getValue("id"), 
                  called, result->get(1,1)->toString().c_str(), limit);
	msg.setParam("error", "busy");
	msg.setParam("reason", "Busy there");
	msg.retValue() = "-";
	return true;	
    }
    lock.drop();
    
    RefObject* data = msg.userData();
    m_hash.append(new Fax2EmailRec(msg.getValue("id"),
                                 result->get(0, 1)->toString(),
                                 result->get(1, 1)->toString(),
				 msg.getValue("caller"),
                                 data));
    
    
    char* tempFile = tempnam("/tmp/fax2email/", "fax");
    msg.retValue() = "fax/receive";
    msg.retValue() += tempFile;
    Debug(&__plugin, DebugMild, "Routed call %s to %s to fax %s (%s). %d calls in list. Result set: %s", msg.getValue("id"), 
                  msg.getValue("called"), msg.retValue().c_str(), result->get(1,1)->toString().c_str(), m_hash.count(), dbg.c_str());    
    free(tempFile);
    return true;
    /*String callto = msg.getValue("callto");
    if (callto.null())
	return false;
    String tmp = callto;
    Lock lock(this);
    if (!callto.startSkip(m_prefix,false))
	return false;
    int sep = callto.find("/");
    if (sep < 0)
	return false;
    String node = callto.substr(0,sep).trimBlanks();
    callto = callto.substr(sep+1);
    if (callto.trimBlanks().null())
	return false;
    DDebug(&__plugin,DebugAll,"Call to '%s' on node '%s'",callto.c_str(),node.c_str());
    // check if the node is to be dynamically allocated
    if ((node == "*") && m_message) {
	Message m(m_message);
	m.addParam("allocate",String::boolText(true));
	m.addParam("nodename",Engine::nodeName());
	m.addParam("callto",callto);
	const char* param = msg.getValue("billid");
	if (param)
	    m.addParam("billid",param);
	param = msg.getValue("username");
	    m.addParam("username",param);
	if (!Engine::dispatch(m) || (m.retValue() == "-") || (m.retValue() == "error")) {
	    const char* error = m.getValue("error","failure");
	    const char* reason = m.getValue("reason");
	    Debug(&__plugin,DebugWarn,"Could not get node for '%s'%s%s%s%s",
		callto.c_str(),
		(error ? ": " : ""), c_safe(error),
		(reason ? ": " : ""), c_safe(reason));
	    if (error)
		msg.setParam("error",error);
	    else
		msg.clearParam("error");
	    if (reason)
		msg.setParam("reason",reason);
	    else
		msg.clearParam("reason");
	    return false;
	}
	node = m.retValue();
	Debug(&__plugin,DebugInfo,"Using node '%s' for '%s'",
	    node.c_str(),callto.c_str());
    }
    msg.setParam("callto",callto);
    // if the call is for the local node just let it through
    if (node.null() || (Engine::nodeName() == node))
	return false;
    if (!node.matches(m_regexp)) {
	msg.setParam("callto",tmp);
	return false;
    }
    String dest = node.replaceMatches(m_callto);
    lock.drop();
    msg.replaceParams(dest);
    if (dest.trimBlanks().null()) {
	msg.setParam("callto",tmp);
	return false;
    }
    Debug(&__plugin,DebugNote,"Call to '%s' on node '%s' goes to '%s'",
	callto.c_str(),node.c_str(),dest.c_str());
    msg.setParam("callto",dest);
    msg.setParam("osip_x-callto",callto);
    msg.setParam("osip_x-billid",msg.getValue("billid"));
    msg.setParam("osip_x-nodename",Engine::nodeName());
    msg.setParam("osip_x-username",msg.getValue("username"));
    return false;*/
}


bool Fax2EmailModule::msgHangup(Message &msg)
{
    String id = msg.getValue("lastpeerid");
    Lock lock(this);
    GenObject *obj = m_hash[id];
    if (!obj)
        return false;
    Fax2EmailRec *rec = static_cast<Fax2EmailRec*>(obj->getObject("Fax2EmailRec"));
    if (!rec)
        return false;
    String called = rec;
    String caller = rec->getFrom();
    String email = rec->getEmail();
    String attach("/");
    attach += msg.getValue("address");
    
    FaxLimit * limObj = 0;
    obj = m_limit[rec];
    if (!obj)
      Debug(&__plugin, DebugWarn, "Can not find limit object for %s", rec->toString().c_str());
    else
      limObj = static_cast<FaxLimit*>(obj->getObject("FaxLimit"));
    if (limObj) {
	if (limObj->unref() <= 0)
	    m_limit.remove(limObj, true);
    }
    m_hash.remove(rec, true);
    lock.drop();
    if (msg.getParam("faxpages")) {
	String subject("Fax from ");
	subject << caller.c_str() << " (" << msg.getValue("faxident_remote") << "), " << msg.getValue("faxpages") << " pages, received by " << called.c_str();
	String body("Faxtype: ");
	body << msg.getValue("faxtype") << "\nFaxECM: " << msg.getValue("faxecm") << "\nFaxCaller: " << msg.getValue("faxcaller");
	send_email(email.c_str(), m_emailFrom.c_str(), subject.c_str(), body.c_str(), attach);
	unlink(attach);
	Debug(&__plugin, DebugMild, "Sent fax from %s to %s. Filename: %s", caller.c_str(), email.c_str(), attach.c_str());
    } else
	Debug(&__plugin, DebugWarn, "Fax from %s has zero pages. File: %s, email: %s", caller.c_str(), attach.c_str(), email.c_str());
    Debug(&__plugin, DebugMild, "Deleted call %s. %d/%d calls remaining", id.c_str(), m_hash.count(), m_limit.count());
    return false;
}

bool Fax2EmailModule::received(Message& msg, int id)
{
    switch (id) {
	case CallRoute:
	    return msgRoute(msg);
	case ChanHangup:
	    return msgHangup(msg);
	default:
	    return Module::received(msg,id);
    }
}

Fax2EmailModule::Fax2EmailModule()
    : Module("fax2email","misc",true),
      m_init(false)
{
    Output("Loaded module Fax2Email");
}

Fax2EmailModule::~Fax2EmailModule()
{
    Output("Unloading module Fax2Email");
}

void Fax2EmailModule::initialize()
{
    Output("Initializing module Fax2Email");
    Configuration cfg(Engine::configFile("fax2email"));
    lock();
    m_account = cfg.getValue("general", "account", "default");
    m_emailFrom = cfg.getValue("general", "emailFrom", "fax@localhost");
    m_callRoutePrio = cfg.getIntValue("priorities", "call.route", 10);
    m_chanHangupPrio = cfg.getIntValue("priorities", "chan.hangup", 10);
    //m_prefix = cfg.getValue("general","prefix","cluster");
    //m_regexp = cfg.getValue("general","regexp");
    //m_callto = cfg.getValue("general","callto");
    //m_message = cfg.getValue("general","locate","cluster.locate");
    //m_handleReg = cfg.getBoolValue("general","user.register",true);
    //m_handleCdr = cfg.getBoolValue("general","call.cdr",true);
    unlock();
    if (!m_init) {
        setup();
        installRelay(CallRoute, "call.route", m_callRoutePrio);
        installRelay(ChanHangup, "chan.hangup", m_chanHangupPrio);
        m_init = true;
    }
}

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */

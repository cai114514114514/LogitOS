/* MessagePort uses the page's existing task queue, whose close invalidates
 * callbacks before the JS context is freed. Endpoint close additionally drops
 * only this destination's queued messages; already-posted messages to a live
 * peer remain valid when their sender closes. Generations fence callbacks,
 * and clearTimeout releases the queued closure rather than retaining it until
 * a later pump. start() schedules delivery; it never dispatches synchronously.
 *
 * Prior implementation captured sender-owned data and passed it through.
 * This path captures the shared serializer once, takes the clone at send time,
 * and refuses non-empty transfer lists because detachment is not implemented.
 * It is an in-page channel, not a claim of cross-origin frame messaging.
 */
"var portClone, portIs;\n"
"if (!G.MessageChannel) {\n"
"  var ports=new WeakMap(), token={}, enqueue=G.setTimeout, dequeue=G.clearTimeout;\n"
"  var state=function(p){var s=ports.get(p);if(!s)throw new TypeError('Illegal MessagePort receiver');return s;};\n"
"  var Port=function MessagePort(key){\n"
"    if(key!==token)throw new TypeError('Illegal constructor');\n"
"    ports.set(this,{owner:this,peer:null,closed:false,generation:0,started:false,queue:[],timer:0,listeners:[],handler:null});\n"
"  };\n"
"  portIs=function(v){return ports.has(v);};\n"
"  function report(e){if(G.reportError)G.reportError(e);}\n"
"  function schedule(s){\n"
"    if(s.closed || !s.started)return;\n"
"    for(var i=0;i<s.queue.length;i++)if(!s.queue[i].timer)scheduleOne(s,s.queue[i]);\n"
"  }\n"
"  function scheduleOne(s,entry){\n"
"    var generation=s.generation;\n"
"    entry.timer=enqueue(function(){\n"
"      entry.timer=0;\n"
"      if(s.closed || s.generation!==generation)return;\n"
"      var index=s.queue.indexOf(entry);if(index<0)return;s.queue.splice(index,1);var data=entry.data;\n"
"      var ev=new G.MessageEvent('message',{data:data,source:null,ports:[]});\n"
"      Object.defineProperty(ev,'target',{value:s.owner,configurable:true});\n"
"      Object.defineProperty(ev,'currentTarget',{value:s.owner,configurable:true});\n"
"      if(typeof s.handler==='function'){try{s.handler.call(s.owner,ev);}catch(e){report(e);}}\n"
"      var listeners=s.listeners.slice();\n"
"      for(var i=0;i<listeners.length;i++){\n"
"        var l=listeners[i];if(s.listeners.indexOf(l)<0)continue;\n"
"        if(l.once)s.listeners.splice(s.listeners.indexOf(l),1);\n"
"        try{if(typeof l.fn==='function')l.fn.call(s.owner,ev);else l.fn.handleEvent(ev);}catch(e){report(e);}\n"
"      }\n"
"      Object.defineProperty(ev,'currentTarget',{value:null,configurable:true});\n"
"      schedule(s);\n"
"    },0);\n"
"    if(!entry.timer)throw new G.DOMException('MessagePort task queue is full','QuotaExceededError');\n"
"  }\n"
"  Port.prototype={\n"
"    constructor:Port,\n"
"    addEventListener:function(type,fn,options){\n"
"      var s=state(this);if(type!=='message'||!fn)return;\n"
"      if(typeof fn!=='function' && typeof fn.handleEvent!=='function')return;\n"
"      for(var i=0;i<s.listeners.length;i++)if(s.listeners[i].fn===fn)return;\n"
"      s.listeners.push({fn:fn,once:!!(options&&options.once)});\n"
"    },\n"
"    removeEventListener:function(type,fn){var s=state(this);if(type!=='message')return;for(var i=0;i<s.listeners.length;i++)if(s.listeners[i].fn===fn){s.listeners.splice(i,1);return;}},\n"
"    start:function(){var s=state(this);if(s.closed)return;s.started=true;schedule(s);},\n"
"    close:function(){\n"
"      var s=state(this);\n"
#ifndef MESSAGE_PORT_NO_CLOSE_FENCE
"      if(s.closed)return;s.closed=true;s.generation++;\n"
"      for(var i=0;i<s.queue.length;i++)if(s.queue[i].timer)dequeue(s.queue[i].timer);s.queue=[];\n"
"      if(s.peer && s.peer.peer===s)s.peer.peer=null;\n"
"      s.peer=null;\n"
#endif
"    },\n"
"    postMessage:function(data,options){\n"
"      var s=state(this);if(arguments.length===0)throw new TypeError('MessagePort.postMessage requires data');\n"
"      var transfer=options && !Array.isArray(options) ? options.transfer : options;\n"
"      if(transfer!==undefined && transfer!==null){\n"
"        if(typeof transfer[Symbol.iterator]!=='function')throw new TypeError('transfer must be iterable');\n"
"        if(Array.from(transfer).length)throw new G.DOMException('MessagePort transfer is not implemented','DataCloneError');\n"
"      }\n"
#ifdef MESSAGE_PORT_NO_CLONE
"      var cloned=data;\n"
#else
"      var cloned=portClone(data);\n"
#endif
"      var peer=s.peer;if(!peer || peer.closed)return;\n"
"      if(peer.queue.length>=4096)throw new G.DOMException('MessagePort queue is full','QuotaExceededError');\n"
"      peer.queue.push({data:cloned,timer:0});\n"
"      try{schedule(peer);}catch(e){peer.queue.pop();throw e;}\n"
"    }\n"
"  };\n"
"  Object.defineProperty(Port.prototype,'onmessage',{get:function(){return state(this).handler;},set:function(fn){var s=state(this);s.handler=typeof fn==='function'?fn:null;if(typeof fn==='function'){s.started=true;schedule(s);}},configurable:true});\n"
"  Object.defineProperty(Port.prototype,Symbol.toStringTag,{value:'MessagePort'});\n"
"  G.MessagePort=Port;\n"
"  G.MessageChannel=function MessageChannel(){this.port1=new Port(token);this.port2=new Port(token);state(this.port1).peer=state(this.port2);state(this.port2).peer=state(this.port1);};\n"
"}\n"

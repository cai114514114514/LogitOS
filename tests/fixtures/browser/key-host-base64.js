/* Test adapter only: the product installs these Web API functions elsewhere. */
const alphabet='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
globalThis.btoa=function(s){let o='';for(let i=0;i<s.length;i+=3){const a=s.charCodeAt(i),b=s.charCodeAt(i+1)||0,c=s.charCodeAt(i+2)||0;o+=alphabet[a>>2]+alphabet[((a&3)<<4)|(b>>4)]+(i+1<s.length?alphabet[((b&15)<<2)|(c>>6)]:'=')+(i+2<s.length?alphabet[c&63]:'=')}return o};
globalThis.atob=function(s){let o='',v=0,bits=0;for(const c of s.replace(/=+$/,'')){const n=alphabet.indexOf(c);if(n<0)throw Error('base64');v=v*64+n;bits+=6;if(bits>=8){bits-=8;o+=String.fromCharCode((v>>bits)&255);v&=(1<<bits)-1}}return o};

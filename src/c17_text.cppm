module;
#include <string>
#include <string_view>
export module parsley.c17_text;
export namespace parsley::c17_text {
inline std::size_t utf8_width(std::string_view text,std::size_t offset) {
  if(offset>=text.size())return 0;
  unsigned char c=static_cast<unsigned char>(text[offset]);
  std::size_t n=c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:1;
  if(n>text.size()-offset)return 1;
  for(std::size_t i=1;i<n;++i)if((static_cast<unsigned char>(text[offset+i])&0xc0)!=0x80)return 1;
  return n;
}
inline std::string module_stem(std::string_view name) {
  std::string out;
  constexpr char hex[]="0123456789abcdef";
  for(unsigned char c:name) {
    if(c>='A'&&c<='Z')out+=char(c-'A'+'a');
    else if((c>='a'&&c<='z')||(c>='0'&&c<='9'))out+=char(c);
    else if(c=='_')out+="__";
    else if(c=='.')out+="_dot_";
    else {out+="_x";out+=hex[c>>4];out+=hex[c&15];out+='_';}
  }
  if(out.empty())out="parser";
  if(out.front()>='0'&&out.front()<='9')out="p_"+out;
  return out;
}
inline std::string display_string(std::string_view text) {
  std::string out="\"";constexpr char hex[]="0123456789abcdef";
  for(unsigned char c:text) {
    if(c=='"')out+="\\\"";else if(c=='\\')out+="\\\\";
    else if(c=='\n')out+="\\n";else if(c=='\r')out+="\\r";else if(c=='\t')out+="\\t";
    else if(c==0)out+="\\0";else if(c<32||c==127){out+="\\x";out+=hex[c>>4];out+=hex[c&15];}
    else out+=char(c);
  }
  return out+'"';
}
inline std::string literal(std::string_view text) {
  std::string out;
  // C17 guarantees only 4095 characters per concatenated string literal.
  if (text.size() > 2048) {
    out="((const char[]){";size_t n=0;
    for(unsigned char c:text){if(n++%64==0)out+='\n';out+="(char)"+std::to_string(c)+",";}
    return out+"0})";
  }
  out+='"';
  for(unsigned char c:text) {
    if(c=='"')out+="\\\"";
    else if(c=='\\')out+="\\\\";
    else if(c=='?')out+="\\?";
    else if(c=='\n')out+="\\n";
    else if(c=='\r')out+="\\r";
    else if(c=='\t')out+="\\t";
    else if(c<32||c>=127){out+='\\';out+=char('0'+((c>>6)&7));out+=char('0'+((c>>3)&7));out+=char('0'+(c&7));}
    else out+=char(c);
  }
  return out+'"';
}
}
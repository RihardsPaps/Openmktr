from pathlib import Path
p=Path('/campaign/tool-debug/src/parser.cc')
s=p.read_text()
s=s.replace('    if (!define_name_.empty()) {\n      ParseInsideDefine(line);', '    if (std::string_view(loc_.filename).find("CustomTarget_images.mk") != std::string_view::npos && loc_.lineno >= 110 && loc_.lineno <= 114)\n      fprintf(stderr, "DEBUG line %d after_rule %d text [%.*s]\\n", loc_.lineno, after_rule_, int(line.size()), line.data());\n    if (!define_name_.empty()) {\n      ParseInsideDefine(line);')
p.write_text(s)

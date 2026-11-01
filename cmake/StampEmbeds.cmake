# Writes a generated translation unit that `#embed`s files: ${TEMPLATE}, plus a
# digest of the bytes of every file in ${EMBEDS}, to ${OUTPUT}. Run by the build
# (cmake -P) after the embedded files exist, so the digest is of this build's files.
#
# The digest is a declaration, not a comment, and that is the point. #embed is not a
# dependency the build tool sees, and ccache keys a translation unit on its
# preprocessed text and the headers it includes, never on the bytes #embed reads:
# with the file changed and the text not, ccache hands back the object it built
# against the old bytes (reproduced with ccache 4.9.1 and clang 22). A comment would
# not do, as the preprocessor drops it before ccache hashes the text. With the digest
# in the text, the translation unit changes exactly when what it embeds does.
#
# The output is always rewritten: an output older than the files it depends on is out
# of date to the build tool on every run, however identical its text.
file(READ "${TEMPLATE}" text)
set(digests "")
foreach(file IN LISTS EMBEDS)
  file(SHA256 "${file}" digest)
  string(APPEND digests "${digest}")
endforeach()
string(SHA256 digest "${digests}")
string(APPEND text "\nnamespace {\n[[maybe_unused]] const char kEmbedDigest[] = \"${digest}\";\n} // namespace\n")

file(WRITE "${OUTPUT}" "${text}")

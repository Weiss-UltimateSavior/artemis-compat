#pragma once
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLbitfield = unsigned int;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x4000;
void glViewport(GLint, GLint, GLsizei, GLsizei);
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void glClear(GLbitfield);

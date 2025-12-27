from setuptools import setup, Extension
import sys

if sys.platform == "win32":
    extra_compile_args = ['/std:c++17']
    python_lib = [f"python{sys.version_info.major}{sys.version_info.minor}", "python3"]
else:
    extra_compile_args = ['-std=c++17']
    python_lib = []

module = Extension(
    'private_attribute',
    sources=['private_attribute_cpp.cpp'],
    include_dirs=['.'],
    language='c++',
    extra_compile_args=extra_compile_args,
    libraries=python_lib,
)

readme = open('README.md').read()

setup(
    name='private_attribute_cpp',
    version='1.0.0',
    description='A Python package that provides a way to define private attributes in C++ implementation.',
    ext_modules=[module],
    zip_safe=False,
    long_description=readme,
    long_description_content_type='text/markdown',
    license="MIT",
    # add "private_attribute.pyi"
    package_data={'': ['private_attribute.pyi']},
)
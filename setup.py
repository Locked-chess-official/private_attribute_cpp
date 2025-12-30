from setuptools import setup, Extension
import sys
import sysconfig

if sys.platform == "win32":
    extra_compile_args = ['/std:c++17']
else:
    extra_compile_args = ['-std=c++17']

module = Extension(
    'private_attribute',
    sources=['private_attribute.cpp'],
    include_dirs=['.'],
    language='c++',
    extra_compile_args=extra_compile_args,
)

readme = open('README.md').read()

setup(
    name='private_attribute_cpp',
    version='1.0.5',
    author="HuangHaoHua",
    author_email="13140752715@example.com",
    description='A Python package that provides a way to define private attributes in C++ implementation.',
    ext_modules=[module],
    zip_safe=False,
    long_description=readme,
    long_description_content_type='text/markdown',
    license="MIT",
    # add "private_attribute.pyi"
    package_data={'': ['private_attribute.pyi']},

    include_package_data=False,
    packages=[""],

    url="https://github.com/Locked-chess-official/private_attribute_cpp"
)
#pragma once

#include <optional>
#include <random>
#include <vector>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

#include "resource.hpp"

// NOTE: To enable more semantic programming, we use this wrapper over the
// standard C++ optional type which can implicitly unpack into its underlying
// type. The value checking capabilities are still present, however.
template <typename T>
struct weakly_optional : std::optional <T> {
	using std::optional <T> ::optional;

	// operator T() const {
	// 	if (!this->has_value()) {
	// 		// TODO: custom logging
	// 		fmt::print("Implicitly converting weakly_optional tensor of null value\n");
	// 		// std::terminate();
	// 		std::exit(EXIT_FAILURE);
	// 	}
	//
	// 	return this->value();
	// }

	// TODO: only if it has an indexing
	T::index_type operator[](int i) {
		// Implicitly unpack
		T value = *this;
		return value[i];
	}
};

struct Shape : std::vector <long int> {
	using parent = std::vector <long int>;

	// Empty shape; a scalar
	Shape() {}

	// From a vector
	template <typename T>
	requires std::is_integral_v <T>
	Shape(const std::vector <T> &v) {
		resize(v.size());

		for (size_t i = 0; i < v.size(); i++)
			(*this)[i] = v[i];
	}

	// Construct from integral initializer list
	// TODO: use optional format, in case there are negative numbers
	template <std::integral T>
	Shape(const std::initializer_list <T> &list) {
		resize(list.size());

		size_t i = 0;
		for (auto it = list.begin(); it != list.end(); it++)
			(*this)[i++] = *it;
	}

	size_t elements() const {
		size_t prod = 1;
		for (size_t i = 0; i < size(); i++)
			prod *= (*this)[i];
		return prod;
	}

	Shape pop() const {
		if (size() < 1)
			return {};

		return std::vector <long int> (std::next(begin()), end());
	}

	// Indexing, with negatives allowed; returns the first index if out of bounds index
	const long int &operator[](int i) const {
		int j = (i > 0) ? i : i + size();
		// TODO: warning
		return (j < size() && j >= 0) ? parent::operator[](j) : parent::operator[](0);
	}

	long int &operator[](int i) {
		int j = (i > 0) ? i : i + size();
		// TODO: warning
		return (j < size() && j >= 0) ? parent::operator[](j) : parent::operator[](0);
	}

	// Reshape, with an optional ambiguous dimension (-1)
	std::optional <Shape> reshape(const Shape &other) const {
		Shape final = other;

		int minus = -1;
		size_t remainder = elements();

		size_t i = 0;
		while (i < other.size()) {
			if (other[i] == -1) {
				if (minus == -1) {
					minus = i++;
					continue;
				} else {
					// Cannot have multiple ambiguous dimensions
					return std::nullopt;
				}
			}

			if (remainder % other[i] != 0)
				return std::nullopt;

			remainder /= other[i++];
		}

		if (minus != -1)
			final[minus] = remainder;
		else if (remainder != 1)
			return std::nullopt;

		return final;
	}

	// Comparison
	bool operator==(const Shape &other) const {
		if (other.size() != size())
			return false;

		for (size_t i = 0; i < size(); i++) {
			if ((*this)[i] != other[i])
				return false;
		}

		return true;
	}
};

struct Tensor {
	Resource buffer;
	std::optional <Shape> shape = std::nullopt; // TODO: make this weakly optional
	long long int tag = -1;

	// Tag generation
	static struct {
		long long int next_tag;

		long long int operator()() {
			return next_tag++;
		}
	} tagger;

	// Type definitions for templates
	using index_type = Tensor;

	// Tracking the memory; debug only
	[[gnu::always_inline]]
	Tensor &track() {
		fmt::print("TRACKING ON FOR @{}\n", (void *) buffer.ptr);
		buffer.tracking = true;
		return *this;
	}

	// Assigning values
	Tensor &operator=(double v) {
		// TODO: type checking
		buffer.memset(v);
		return *this;
	}

	// Indexing the topmost dimension
	// TODO: negative dimensions as well...
	Tensor operator[](size_t i) const {
		// TODO: error
		if (!shape || i >= (*shape)[0])
			return {};

		Shape sub_shape = shape->pop();
		size_t start = i * sub_shape.elements();
		size_t end = start + sub_shape.elements();
		Resource sub_buffer = buffer.slice(start, end).value();
		return Tensor { sub_buffer, sub_shape, tagger() };
	}

	// Reshaping tensors
	Tensor reshape(const Shape &other) const {
		if (auto reshaped = shape->reshape(other)) {
			Resource reshaped_buffer = *buffer.slice(); // Gets the whole thing for free
			return Tensor { reshaped_buffer, *reshaped, tagger() };
		}

		// TODO: error
		fmt::print("reshape error; incompatible shapes\n");
		std::abort();

		return {};
	}

	template <std::integral ... Ts>
	Tensor reshape(Ts ... sizes) const {
		std::initializer_list <long int> other { (long int) sizes... };
		return reshape(other);
	}

	// Transposing 2D tensors
	Tensor transpose() const {
		if (shape->size() != 2)
			return {};

		size_t rows = shape.value()[0];
		size_t cols = shape.value()[1];
		Shape transposed_shape { cols, rows };
		Tensor transposed = Tensor::blank(transposed_shape);

		// Write the elements
		for (size_t i = 0; i < rows; i++) {
			for (size_t j = 0; j < cols; j++)
				transposed.buffer.ptr[j * rows + i] = buffer.ptr[i * cols + j];
		}

		return transposed;
	}

	// Copy tensor data
	bool copy(const Tensor &other) {
		if (shape != other.shape)
			return false;

		return buffer.copy(other.buffer);
	}

	// Cloning tensors; does not transfer tracking
	Tensor clone() const {
		Resource cloned_buffer = *buffer.clone();
		return Tensor { cloned_buffer, shape, tagger() };
	}

	// Slicing through a single dimension
	Tensor slice(size_t start, size_t end, size_t dim = 0) {
		// TODO: allow negatives
		if (dim >= shape->size())
			return {};

		// NOTE: End is not inclusive
		if (start >= end || start > shape.value()[dim] || end > shape.value()[dim])
			return {};

		size_t prod_before = 1;
		size_t prod_after = 1;
		for (size_t i = 0; i < shape->size(); i++) {
			prod_before *= (i < dim) ? shape.value()[i] : 1;
			prod_after *= (i > dim) ? shape.value()[i] : 1;
		}

		Shape sliced_shape = *shape;
		sliced_shape[dim] = end - start;

		Tensor sliced = Tensor::blank(sliced_shape);
		for (size_t i = 0; i < prod_before; i++) {
			// TODO: put k in the inner loop?
			for (size_t j = 0; j < end - start; j++) {
				for (size_t k = 0; k < prod_after; k++) {
					size_t ithis = i * shape.value()[dim] * prod_after + j * prod_after + k;
					size_t isliced = i * (end - start) * prod_after + j * prod_after + k;
					sliced.buffer.ptr[isliced] = buffer.ptr[ithis];
				}
			}
		}

		return sliced;
	}

	// Blank tensor of a given shape; no memset-ing
	static Tensor blank(const Shape &shape, Resource::Type type = Resource::Type::f32, Resource::Device device = Resource::Device::eCPU) {
		if (auto buffer = Resource::from(shape.elements(), type, device))
			return Tensor { *buffer, shape, tagger() };

		return {};
	}

	static Tensor blank_like(const Tensor &t) {
		if (auto buffer = Resource::from(t.shape.value().elements(), t.buffer.type, t.buffer.device))
			return Tensor { *buffer, t.shape.value(), tagger() };

		return {};
	}

	// Zero tensor
	static Tensor zeros(const Shape &shape, Resource::Type type = Resource::Type::f32, Resource::Device device = Resource::Device::eCPU) {
		if (auto buffer = Resource::from(shape.elements(), type, device)) {
			buffer->memset(0.0f);
			return Tensor { *buffer, shape, tagger() };
		}

		return {};
	}

	static Tensor zeros_like(const Tensor &t) {
		if (auto buffer = Resource::from(t.shape.value().elements(), t.buffer.type, t.buffer.device)) {
			buffer->memset(0.0f);
			return Tensor { *buffer, t.shape.value(), tagger() };
		}

		return {};
	}

	// One tensor
	static Tensor ones(const Shape &shape, Resource::Type type = Resource::Type::f32, Resource::Device device = Resource::Device::eCPU) {
		if (auto buffer = Resource::from(shape.elements(), type, device)) {
			buffer->memset(1.0f);
			return Tensor { *buffer, shape, tagger() };
		}

		return {};
	}

	static Tensor ones_like(const Tensor &t) {
		if (auto buffer = Resource::from(t.shape.value().elements(), t.buffer.type, t.buffer.device)) {
			buffer->memset(1.0f);
			return Tensor { *buffer, t.shape.value(), tagger() };
		}

		return {};
	}

	// Identity tensor
	// TODO: expand for multidim tensors
	static Tensor identity(size_t N, Resource::Type type = Resource::Type::f32, Resource::Device device = Resource::Device::eCPU) {
		Shape shape { N, N };
		if (auto buffer = Resource::from(shape.elements(), type, device)) {
			buffer->memset(0.0f);
			for (size_t i = 0; i < N; i++)
				buffer->ptr[i * N + i] = 1.0f;
			return Tensor { *buffer, shape, tagger() };
		}

		return {};
	}

	// Tensor concatenation; explicit dimension must be provided
	static Tensor concat(const Tensor &A, const Tensor &B, size_t dim) {
		// TODO: allow for negative dim (int)

		// Make sure the shapes match except for the provided dimension
		// TODO: unpack the shapes here
		if (A.shape->size() != B.shape->size())
			return {};

		size_t sum = 0;
		size_t nA = 0;
		size_t nB = 0;
		size_t prod_before = 1;
		size_t prod_after = 1;
		for (size_t i = 0; i < A.shape->size(); i++) {
			if (i == dim) {
				nA = A.shape.value()[i];
				nB = B.shape.value()[i];
				sum = nA + nB;
			} else if (A.shape.value()[i] != B.shape.value()[i]) {
				return {};
			} else {
				prod_before *= (sum == 0) ? A.shape.value()[i] : 1;
				prod_after *= (sum > 0) ? A.shape.value()[i] : 1;
			}
		}

		Shape cat_shape = *A.shape;
		cat_shape[dim] = sum;

		Tensor out = Tensor::zeros(cat_shape);

		const Resource &rA = A.buffer;
		const Resource &rB = B.buffer;
		Resource &rout = out.buffer;

		for (size_t i = 0; i < prod_before; i++) {
			// TODO: put k in the inner loop?
			for (size_t j = 0; j < nA; j++) {
				for (size_t k = 0; k < prod_after; k++) {
					size_t iA = i * nA * prod_after + j * prod_after + k;
					size_t iout = i * sum * prod_after + j * prod_after + k;
					rout.ptr[iout] = rA.ptr[iA];
				}
			}

			for (size_t j = 0; j < nB; j++) {
				for (size_t k = 0; k < prod_after; k++) {
					size_t iB = i * nB * prod_after + j * prod_after + k;
					size_t iout = i * sum * prod_after + (j + nA) * prod_after + k;
					rout.ptr[iout] = rB.ptr[iB];
				}
			}
		}

		return out;
	}

	// TODO: stack (new dim) and cat (dim=-1)

	// Random matrix initializations
	// TODO: dissociate from tensors
	static Tensor xavier(size_t in, size_t out, Resource::Type type = Resource::Type::f32, Resource::Device device = Resource::Device::eCPU) {
		Shape shape { in, out };
		if (auto buffer = Resource::from(shape.elements(), type, device)) {
			std::random_device rd;
			std::mt19937 generator(rd());
			std::normal_distribution <> distribution(0, std::sqrt(1.0/(in + out)));
			// TODO: defer then
			for (size_t i = 0; i < shape.elements(); i++)
				buffer->ptr[i] = distribution(generator);
			return Tensor { *buffer, shape, tagger() };
		}

		return {};
	}

	// Random tensor
	static Tensor randn(const Shape &shape, Resource::Type type = Resource::Type::f32, Resource::Device device = Resource::Device::eCPU) {
		if (auto buffer = Resource::from(shape.elements(), type, device)) {
			std::random_device rd;
			std::mt19937 generator(rd());
			std::uniform_real_distribution <> distribution(0, 1);
			for (size_t i = 0; i < shape.elements(); i++)
				buffer->ptr[i] = distribution(generator);
			return Tensor { *buffer, shape, tagger() };
		}

		return {};
	}
};

// Printing tensors
std::string format_as(const Shape &);
std::string format_as(const Tensor &);

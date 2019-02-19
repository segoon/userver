#include <storages/mongo_ng/collection.hpp>

#include <storages/mongo_ng/bson_error.hpp>
#include <storages/mongo_ng/collection_impl.hpp>
#include <storages/mongo_ng/cursor_impl.hpp>

namespace storages::mongo_ng {

Collection::Collection(std::shared_ptr<impl::CollectionImpl> impl)
    : impl_(std::move(impl)) {}

Cursor Collection::Find(const formats::bson::Document& filter) const {
  auto[client, collection] = impl_->GetNativeCollection();
  impl::CursorPtr native_cursor(mongoc_collection_find_with_opts(
      collection.get(), filter.GetBson().get(), nullptr, nullptr));
  return Cursor(impl::CursorImpl(std::move(client), std::move(native_cursor)));
}

size_t Collection::CountApprox() const {
  auto[client, collection] = impl_->GetNativeCollection();

  impl::BsonError error;
  auto count = mongoc_collection_estimated_document_count(
      collection.get(), nullptr, nullptr, nullptr, error.Get());
  if (count < 0) error.Throw();
  return count;
}

size_t Collection::Count(const formats::bson::Document& filter) const {
  auto[client, collection] = impl_->GetNativeCollection();

  impl::BsonError error;
  auto count = mongoc_collection_count_documents(
      collection.get(), filter.GetBson().get(), nullptr, nullptr, nullptr,
      error.Get());
  if (count < 0) error.Throw();
  return count;
}

void Collection::InsertOne(const formats::bson::Document& document) {
  auto[client, collection] = impl_->GetNativeCollection();

  impl::BsonError error;
  if (!mongoc_collection_insert_one(collection.get(), document.GetBson().get(),
                                    nullptr, nullptr, error.Get())) {
    error.Throw();
  }
}

}  // namespace storages::mongo_ng

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/unmarshaller/unmarshaller.h"

namespace xe {
namespace kernel {
namespace xam {

Unmarshaller::Unmarshaller(uint32_t marshaller_address)
    : xlivebase_async_message_ptr_(nullptr),
      xlive_async_task_ptr_(nullptr),
      schema_data_ptr_(nullptr),
      ordinal_to_index_ptr_(nullptr),
      schema_table_entry_ptr_(nullptr),
      data_ptr_({}),
      url_offsets_ptr(nullptr),
      url_data_ptr(nullptr),
      constant_list_ptr(nullptr),
      position_(0) {
  if (!marshaller_address) {
    return;
  }

  xlivebase_async_message_ptr_ =
      kernel_state()->memory()->TranslateVirtual<XLIVEBASE_ASYNC_MESSAGE*>(
          marshaller_address);

  xlive_async_task_ptr_ =
      kernel_state()->memory()->TranslateVirtual<XLIVE_ASYNC_TASK*>(
          xlivebase_async_message_ptr_->xlive_async_task_ptr);

  schema_data_ptr_ = kernel_state()->memory()->TranslateVirtual<SCHEMA_DATA*>(
      xlive_async_task_ptr_->schema_data_ptr);

  ordinal_to_index_ptr_ =
      kernel_state()->memory()->TranslateVirtual<ORDINAL_TO_INDEX*>(
          schema_data_ptr_->OrdinalToIndexPtr);

  schema_table_entry_ptr_ =
      kernel_state()->memory()->TranslateVirtual<SCHEMA_TABLE_ENTRY*>(
          schema_data_ptr_->TableEntriesPtr);

  url_offsets_ptr =
      kernel_state()->memory()->TranslateVirtual<xe::be<uint16_t>*>(
          schema_data_ptr_->UrlOffsetsPtr);

  url_data_ptr = kernel_state()->memory()->TranslateVirtual<char*>(
      schema_data_ptr_->UrlDataPtr);

  constant_list_ptr =
      kernel_state()->memory()->TranslateVirtual<xe::be<uint32_t>*>(
          schema_data_ptr_->ConstantListPtr);

  uint8_t* data_request_ptr =
      kernel_state()->memory()->TranslateVirtual<uint8_t*>(
          xlive_async_task_ptr_->marshalled_request_ptr);

  data_ptr_ = std::span<uint8_t>(
      data_request_ptr, xlive_async_task_ptr_->marshalled_request_size);

  XELOGD(
      "\n***************** Unmarshaller Info *****************\n"
      "SchemaVersionMajor: {}\n"
      "SchemaVersionMinor: {}\n"
      "ToolVersion: {:08X}\n"
      "TaskFlags: {:08X}\n"
      "SchemaTableEntries: {}\n"
      "OrdinalToIndexPtr: {:08X}\n"
      "SechmaIndex: {:04X}\n"
      "MarshalledRequestPtr: {:08X}\n"
      "MarshalledRequestSize: {}\n"
      "ResultsPtr: {:08X}\n"
      "RequestsSize: {}\n",
      schema_data_ptr_->Header.SchemaVersionMajor.get(),
      schema_data_ptr_->Header.SchemaVersionMinor.get(),
      schema_data_ptr_->Header.ToolVersion.get(),
      xlive_async_task_ptr_->task_flags.get(),
      schema_data_ptr_->Header.SchemaTableEntries.get(),
      schema_data_ptr_->OrdinalToIndexPtr.get(),
      xlive_async_task_ptr_->schema_index.get(),
      xlive_async_task_ptr_->marshalled_request_ptr.get(),
      xlive_async_task_ptr_->marshalled_request_size.get(),
      xlive_async_task_ptr_->results_ptr.get(),
      xlive_async_task_ptr_->results_size.get());
}

std::span<uint8_t> Unmarshaller::Advance(size_t count) {
  const size_t offset = position_ + count;

  if (offset > data_ptr_.size()) {
    assert_always(std::format("{}: Out of Bounds Span!", __func__));

    return std::span<uint8_t>();
  }

  std::span<uint8_t> data = data_ptr_.subspan(position_, count);

  position_ = offset;

  return data;
}

std::u16string Unmarshaller::ReadSwapUTF16String(uint32_t length) {
  // Excludes null terminator
  std::u16string server_path = xe::load_and_swap<std::u16string>(
      reinterpret_cast<char16_t*>(data_ptr_.data() + position_));

  std::span<uint8_t> string_data =
      Advance(xe::string_util::size_in_bytes(server_path, true));

  assert_false(length != server_path.length() + 1);

  if (string_data.empty()) {
    return u"";
  }

  return server_path;
}

std::string Unmarshaller::ReadString(uint32_t length) {
  std::span<uint8_t> string_data = Advance(length);

  if (string_data.empty()) {
    return "";
  }

  return std::string(reinterpret_cast<char*>(string_data.data()), length);
}

X_HRESULT Unmarshaller::Deserialize() { return X_E_FAIL; }

bool Unmarshaller::GetSchemaEntry(uint16_t schema_index,
                                  SCHEMA_TABLE_ENTRY* schema_entry_ptr) {
  if (schema_index >= GetSchemaData()->Header.SchemaTableEntries) {
    return false;
  }

  std::memcpy(schema_entry_ptr, schema_table_entry_ptr_ + schema_index,
              sizeof(SCHEMA_TABLE_ENTRY));

  return true;
}

bool Unmarshaller::GetSchemaDataFromEntry(SCHEMA_TABLE_ENTRY* schema_entry_ptr,
                                          bool request,
                                          std::span<uint8_t>* schema_data) {
  uint32_t request_schema_offset = 0;
  uint32_t schema_data_size = 0;

  if (request) {
    schema_data_size = schema_entry_ptr->RequestSchemaSize;
    request_schema_offset = schema_entry_ptr->RequestSchemaOffset;
  } else {
    schema_data_size = schema_entry_ptr->ResponseSchemaSize;
    request_schema_offset = schema_entry_ptr->ResponseSchemaOffset;
  }

  uint8_t* schema_data_ptr =
      kernel_state()->memory()->TranslateVirtual<uint8_t*>(
          schema_data_ptr_->SchemaDataPtr);

  uint8_t* buffer_ptr = &schema_data_ptr[request_schema_offset];

  *schema_data = std::span<uint8_t>(buffer_ptr, schema_data_size);

  return true;
}

bool Unmarshaller::EndianBufferBind(BASE_ENDIAN_BUFFER* base,
                                    std::span<uint8_t>& buffer) {
  base->BufferPtr = kernel_state()->memory()->HostToGuestVirtual(
      std::to_address(buffer.data()));
  base->BufferSize = static_cast<uint32_t>(buffer.size());
  base->AvailableSize = static_cast<uint32_t>(buffer.size());
  base->ConsumedSize = 0;
  base->ReverseEndian = 1;

  return true;
}

bool Unmarshaller::XLookupSchemaIndexFromOrdinal(uint16_t ordinal,
                                                 uint16_t* schema_index) const {
  auto CompareOrdinal = [](const void* e1, const void* e2) -> int {
    const auto* element1 = reinterpret_cast<const xe::be<uint16_t>*>(e1);
    const auto* element2 = reinterpret_cast<const xe::be<uint16_t>*>(e2);

    return *element1 - *element2;
  };

  ordinal = xe::byte_swap(ordinal);

  const ORDINAL_TO_INDEX* found = static_cast<ORDINAL_TO_INDEX*>(std::bsearch(
      &ordinal, ordinal_to_index_ptr_,
      schema_data_ptr_->Header.SchemaTableEntries, 4, CompareOrdinal));

  if (!found) {
    return false;
  }

  *schema_index = found->Index;

  return true;
}

bool Unmarshaller::LookupUrlFromTable(uint16_t url_index,
                                      std::string_view* url_ptr) {
  if (url_index > GetSchemaData()->Header.UrlTableSize) {
    return false;
  }

  char* url_data_ptr = kernel_state()->memory()->TranslateVirtual<char*>(
      schema_data_ptr_->UrlDataPtr);

  uint16_t url_offset = url_offsets_ptr[url_index];

  if (url_offset > schema_data_ptr_->Header.UrlTableDataSize) {
    return false;
  }

  *url_ptr = std::string_view(url_data_ptr + url_offset);

  return true;
};

bool Unmarshaller::LookupConstantFromTable(uint16_t constant_index,
                                           uint32_t* value_ptr) {
  if (constant_index > schema_data_ptr_->Header.ConstantsTableSize) {
    return false;
  }

  assert_false(schema_data_ptr_->Header.ConstantSize != 4);

  *value_ptr = constant_list_ptr[constant_index];

  return true;
}

void Unmarshaller::PrettyPrintSchemaTables() const {
  const auto schema_table_entries = std::vector<SCHEMA_TABLE_ENTRY>(
      schema_table_entry_ptr_,
      schema_table_entry_ptr_ + schema_data_ptr_->Header.SchemaTableEntries);

  for (uint32_t i = 0; const auto& entry : schema_table_entries) {
    std::string details = fmt::format(
        "Schema entry {}: Request [{:08X}, {:08X}, {:08X}], Response [{:08X}, "
        "{:08X}, "
        "{:08X}], Service: {} ({})\n",
        i, entry.RequestSchemaSize.get(), entry.ResponseSchemaSize.get(),
        entry.RequestSchemaOffset.get(), entry.ResponseSchemaOffset.get(),
        entry.MaxRequestAggregateSize.get(),
        entry.MaxResponseAggregateSize.get(), entry.ServiceIDIndex.get(),
        entry.RequestUrlIndex.get());

    i++;
  }
}

void Unmarshaller::PrettyPrintUrls() {
  const auto schema_table_entries = std::vector<SCHEMA_TABLE_ENTRY>(
      schema_table_entry_ptr_,
      schema_table_entry_ptr_ + schema_data_ptr_->Header.SchemaTableEntries);

  std::string pretty_urls = fmt::format(
      "\nSchema Version: {}.{}\n",
      static_cast<uint32_t>(schema_data_ptr_->Header.SchemaVersionMajor),
      static_cast<uint32_t>(schema_data_ptr_->Header.SchemaVersionMinor));

  for (const auto& entry : schema_table_entries) {
    std::string_view url = "";

    LookupUrlFromTable(entry.RequestUrlIndex, &url);

    pretty_urls.append(fmt::format("URL: {}\n", url));
  }

  XELOGI(pretty_urls);
}

void Unmarshaller::PrettyPrintUrlsWithSchemaIndex() {
  const auto schema_table_entries = std::vector<SCHEMA_TABLE_ENTRY>(
      schema_table_entry_ptr_,
      schema_table_entry_ptr_ + schema_data_ptr_->Header.SchemaTableEntries);

  const auto ordinal_to_index_entries = std::vector<ORDINAL_TO_INDEX>(
      ordinal_to_index_ptr_,
      ordinal_to_index_ptr_ + schema_data_ptr_->Header.SchemaTableEntries);

  std::string pretty_urls = fmt::format(
      "\nSchema Version: {}.{}\n",
      static_cast<uint32_t>(schema_data_ptr_->Header.SchemaVersionMajor),
      static_cast<uint32_t>(schema_data_ptr_->Header.SchemaVersionMinor));

  for (const auto& ordinal_entry : ordinal_to_index_entries) {
    SCHEMA_TABLE_ENTRY schema_entry = {};
    std::string_view url = "";

    GetSchemaEntry(ordinal_entry.Index.get(), &schema_entry);
    LookupUrlFromTable(schema_entry.RequestUrlIndex, &url);

    pretty_urls.append(fmt::format("Schema Index: {:04X}, URL: {}\n",
                                   ordinal_entry.Index.get(), url));
  }

  XELOGI(pretty_urls);
}

void Unmarshaller::PrettyPrintOrdinalToIndex() const {
  const auto ordinal_to_index_entries = std::vector<ORDINAL_TO_INDEX>(
      ordinal_to_index_ptr_,
      ordinal_to_index_ptr_ + schema_data_ptr_->Header.SchemaTableEntries);

  std::string pretty_schema_table = fmt::format(
      "\nSchema Version: {}.{}\n",
      static_cast<uint32_t>(schema_data_ptr_->Header.SchemaVersionMajor),
      static_cast<uint32_t>(schema_data_ptr_->Header.SchemaVersionMinor));

  for (const auto& entry : ordinal_to_index_entries) {
    pretty_schema_table.append(fmt::format("Ordinal: {:04X}, Index: {:04X}\n",
                                           static_cast<uint16_t>(entry.Ordinal),
                                           static_cast<uint16_t>(entry.Index)));
  }

  XELOGI(pretty_schema_table);
}

XLIVEBASE_ASYNC_MESSAGE* Unmarshaller::GetXLiveBaseAsyncMessage() {
  return xlivebase_async_message_ptr_;
}

XLIVE_ASYNC_TASK* Unmarshaller::GetXLiveAsyncTask() {
  return xlive_async_task_ptr_;
}

SCHEMA_DATA* Unmarshaller::GetSchemaData() { return schema_data_ptr_; }

size_t Unmarshaller::GetPosition() const { return position_; }

}  // namespace xam
}  // namespace kernel
}  // namespace xe

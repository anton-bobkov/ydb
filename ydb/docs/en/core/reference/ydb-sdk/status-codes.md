# Status codes

When an error occurs, {{ ydb-short-name }} SDK returns an error object that includes status codes. The returned status code may come from the {{ ydb-short-name }} server, gRPC transport, or the SDK itself.

Status codes within the range of 400000-400999 are {{ ydb-short-name }} server codes that are identical for all {{ ydb-short-name }} SDKs. Refer to [{#T}](./ydb-status-codes.md).

Status codes within the range of 401000-401999 are SDK-specific codes. For more information about SDK-specific codes, refer to the corresponding SDK documentation.

For more information about gRPC status codes, see the [gRPC documentation](https://grpc.io/docs/guides/status-codes/).

## See also

[Questions and answers: Errors](../../faq/errors.md)

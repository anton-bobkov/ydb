# Overloaded shard example

You were notified that your system started taking too long to process user requests.

## Initial problem

Let's take a look at the **Latency** diagrams in the [DB overview](../../reference/observability/metrics/grafana-dashboards.md#dboverview) Grafana dashboard to see if the problem has to do with the {{ ydb-short-name }} cluster:


![DB Overview > Latencies > RW tx server latency](_assets/overloaded-shard/incident-rw-latency.png)

![DB Overview > Latencies > RW tx server latency](_assets/overloaded-shard/incident-latency-percentiles.png)

Indeed, the latencies have increased. Now we need to localize the problem.

## Diagnostics

Let's find out the reason for the latencies to increase. Perhaps, the reason is the increased workload? Here is the **API details** section of the [DB overview](../../reference/observability/metrics/grafana-dashboards.md#dboverview) Grafana dashboard:

![API details](./_assets/overloaded-shard/dboverview-api-details.png)

The number of user requests has definetely increased. But can {{ ydb-short-name }} handle the increased load without additional hardware resources? See the CPU Grafana dashboard:

![CPU](./_assets/overloaded-shard/incident-cpu-dashboard.png)

We can also see the overall CPU usage on the **Diagnostics** tab of the [Embedded UI](../../reference/embedded-ui/index.md):

![CPU diagnostics](./_assets/overloaded-shard/incient-diagnostics-cpu.png)

It looks like the {{ ydb-short-name }} cluster is not utilizing all of its cpu capacity.

If we look at the **DataShard** section [DB overview](../../reference/observability/metrics/grafana-dashboards.md#dboverview) Grafana dashboard? we can see that after the load on the cluster increased, one of its data shards got overloaded.

![Throughput](./_assets/overloaded-shard/incident-datashard-throughput.png)

![Overloaded shard](./_assets/overloaded-shard/incident-datashard-overloaded.png)

To determine what table the overloaded data shard is processing, let's open the **Diagnostics > Top shards** tab in the Embedded UI:

![Diagnostics > shards](./_assets/overloaded-shard/incident-top-shards.png)

See that one of data shards that processes queries for the `stock` table is loaded by 94%.

Let's take a look at the `stock` table on the **Info** tab:

![stock table info](./_assets/overloaded-shard/incident-stock-table-info.png)

{% note warning %}

The `stock` table was created with partitioning by size and by load disabled and has only one partition.

It means that only one data shard processes requests to this talbe. And we know that a data shard can process only one request at a time. This is really bad practice.

{% endnote %}

## Solution

We should enable partitioning by size and by load for the `stock` table:

1. In the Embedded UI, select the database.
2. Open the **Query** tab.
3. Run the following query:

    ```sql
    ALTER TABLE stock SET (
        AUTO_PARTITIONING_BY_SIZE = ENABLED,
        AUTO_PARTITIONING_BY_LOAD = ENABLED
    );
    ```

## Aftermath

As soon as we enable automatic partitioning for the `stock` table, the overloaded data shard start splitting.

![shard distribution by load](./_assets/overloaded-shard/aftermath-shard-distribution-by-load.png)

In five minutes the number of data shards processing the table stabilizes. Multiple data shards are processing queries to the `stock` table now, none of them are overloaded:

![overloaded shard count](./_assets/overloaded-shard/aftermath-datashard-overloaded.png)

![final latency percentiles](./_assets/overloaded-shard/aftermath-latency-percentiles.png)
![final latencies](./_assets/overloaded-shard/aftermath-latencies.png)

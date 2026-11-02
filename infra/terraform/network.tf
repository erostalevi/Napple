# A VPC of its own with one public subnet (no NAT gateway, no cost): the instance reaches ECR, SSM and CloudWatch through the internet gateway.
locals {
  name = "napple-servers"
  az   = "${var.region}${var.az_suffix}"
}

resource "aws_vpc" "main" {
  cidr_block           = var.vpc_cidr
  enable_dns_support   = true
  enable_dns_hostnames = true
  tags                 = { Name = local.name }
}

resource "aws_internet_gateway" "main" {
  vpc_id = aws_vpc.main.id
  tags   = { Name = local.name }
}

resource "aws_subnet" "public" {
  vpc_id            = aws_vpc.main.id
  cidr_block        = cidrsubnet(var.vpc_cidr, 8, 1)
  availability_zone = local.az
  tags              = { Name = "${local.name}-public" }
}

resource "aws_route_table" "public" {
  vpc_id = aws_vpc.main.id
  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.main.id
  }
  tags = { Name = "${local.name}-public" }
}

resource "aws_route_table_association" "public" {
  subnet_id      = aws_subnet.public.id
  route_table_id = aws_route_table.public.id
}

# Inbound: HTTP(S) for Caddy (80 is needed for the ACME HTTP-01 challenge and the HTTPS redirect; NEX rides on 443 as WebSocket), the room port,
# and napple-online's Pia NAT-check UDP ports (IPv4 and IPv6).
# No SSH: shell access is SSM Session Manager (`aws ssm start-session --target <instance id>`).
resource "aws_security_group" "servers" {
  name        = local.name
  description = "Napple game servers: HTTPS API + NEX over WebSocket, room relay, NAT check"
  vpc_id      = aws_vpc.main.id
  tags        = { Name = local.name }

  # A changed description forces replacement, and AWS cannot delete a group the running instance uses: keep the existing one.
  lifecycle {
    ignore_changes = [description]
  }
}

locals {
  ingress = {
    http     = { proto = "tcp", from = 80, to = 80 }
    https    = { proto = "tcp", from = 443, to = 443 }
    https_h3 = { proto = "udp", from = 443, to = 443 }
    room_tcp = { proto = "tcp", from = var.room_port, to = var.room_port }
    room_udp = { proto = "udp", from = var.room_port, to = var.room_port }
  }
}

resource "aws_vpc_security_group_ingress_rule" "v4" {
  for_each          = local.ingress
  security_group_id = aws_security_group.servers.id
  description       = each.key
  ip_protocol       = each.value.proto
  from_port         = each.value.from
  to_port           = each.value.to
  cidr_ipv4         = "0.0.0.0/0"
}

resource "aws_vpc_security_group_ingress_rule" "natcheck" {
  for_each = merge(
    { for p in var.natcheck_udp_ports : "${p}-v4" => { port = p, v4 = "0.0.0.0/0", v6 = null } },
    { for p in var.natcheck_udp_ports : "${p}-v6" => { port = p, v4 = null, v6 = "::/0" } },
  )
  security_group_id = aws_security_group.servers.id
  description       = "natcheck ${each.key}"
  ip_protocol       = "udp"
  from_port         = each.value.port
  to_port           = each.value.port
  cidr_ipv4         = each.value.v4
  cidr_ipv6         = each.value.v6
}

resource "aws_vpc_security_group_egress_rule" "all" {
  security_group_id = aws_security_group.servers.id
  description       = "outbound (ECR, SSM, CloudWatch, ACME, OS updates)"
  ip_protocol       = "-1"
  cidr_ipv4         = "0.0.0.0/0"
}
